/*
 * Virtual ARM7 wireless: a DS radio with nobody else around.
 *
 * On the DS the wireless hardware belongs to the ARM7. The game's WM
 * library (NitroSDK's ARM9 side, built as it is) sends each request to it
 * through the PXI FIFO: the address of a small command buffer. The ARM7
 * carries it out, keeps the shared WMStatus up to date (its state, the
 * parent parameters, the MP buffers) and answers with the address of a
 * callback record, which the library hands to the game's callback. WVR, the
 * library that loads the wireless driver into VRAM for the ARM7, asks the
 * same way first.
 *
 * The port has no DS radio to talk to, but the Underground (and the Union
 * Room, and the other wireless menus) can't start without one: the game
 * stops itself when WVR doesn't start (WirelessDriver_Init). So this plays
 * the ARM7's part for a radio that works and never finds anyone: a parent
 * starts and sends its beacons, a scan finds no parent, every channel is
 * quiet. The Underground then plays as on a DS with nobody nearby, alone.
 *
 * Answers come back as the ARM7's would, later and in order: each is due a
 * number of V-blanks after its request (a scan after its channel time) and
 * is delivered at that V-blank (NitroPort_WmVBlank), never from inside the
 * request, since the game asks for the next scan from inside the last
 * one's callback.
 */
#include <nitro.h>
#include <nitro_port.h>
#include <nitro/wm.h>
#include <nitro/wvr/common/wvr_common.h>

#include <string.h>

/* Answer records: handed to the ARM9 by address, so each stays untouched
 * until it has been delivered. More than can ever be waiting at once. */
#define WM_REPLIES 64

typedef struct PendingReply {
    u32 due;       /* the V-blank it's delivered at */
    int tag;       /* PXI_FIFO_TAG_WM or PXI_FIFO_TAG_WVR */
    u32 data;      /* WM: the record's address; WVR: the result word */
} PendingReply;

static u32 sReplyBufs[WM_REPLIES][WM_FIFO_BUF_SIZE / sizeof(u32)] ATTRIBUTE_ALIGN(32);
static u32 sNextBuf;
static PendingReply sQueue[WM_REPLIES];
static u32 sQueueHead;
static u32 sQueueCount;
static u32 sVBlank;

/* The ARM9's shared buffers (from WM_Initialize / WM_Enable). */
static WMArm7Buf *sWm7;
static WMStatus *sStatus;
/* (a parent) the next beacon's V-blank */
static u32 sNextBeacon;
static BOOL sReported[WM_APIID_ASYNC_KIND_MAX];

static u32 MsToVBlanks(u32 ms)
{
    u32 v = (ms * 60u + 999u) / 1000u;
    return v ? v : 1;
}

static void Queue(int tag, u32 data, u32 delay)
{
    PendingReply *p;
    u32 due = sVBlank + (delay ? delay : 1);

    if (sQueueCount == WM_REPLIES) {
        NitroPort_Log("[wm] answer queue full: answer dropped");
        return;
    }
    /* In order: never before the answer queued ahead of it. */
    if (sQueueCount) {
        const PendingReply *last = &sQueue[(sQueueHead + sQueueCount - 1) % WM_REPLIES];
        if ((s32)(due - last->due) < 0) {
            due = last->due;
        }
    }
    p = &sQueue[(sQueueHead + sQueueCount) % WM_REPLIES];
    p->due = due;
    p->tag = tag;
    p->data = data;
    sQueueCount++;
}

/* A cleared answer record, its apiid and errcode filled in. */
static void *NewReply(u16 apiid, u16 errcode)
{
    WMCallback *cb = (WMCallback *)sReplyBufs[sNextBuf];

    sNextBuf = (sNextBuf + 1) % WM_REPLIES;
    memset(cb, 0, WM_FIFO_BUF_SIZE);
    cb->apiid = apiid;
    cb->errcode = errcode;
    return cb;
}

static void Send(void *reply, u32 delay)
{
    Queue(PXI_FIFO_TAG_WM, (u32)(uintptr_t)reply, delay);
}

static void Plain(u16 apiid, u16 errcode)
{
    Send(NewReply(apiid, errcode), 1);
}

/* Sizes the MP buffers need, as the ARM7 works them out from the
 * parent parameters (what WM_GetMPSendBufferSize and
 * WM_GetMPReceiveBufferSize read back). */
static void SetMpSizes(void)
{
    const WMParentParam *pp = &sStatus->pparam;
    u16 ks = pp->KS_Flag ? 1 : 0;

    sStatus->mp_parentMaxSize = pp->parentMaxSize;
    sStatus->mp_childMaxSize = pp->childMaxSize;
    sStatus->mp_maxSendSize = (u16)(pp->parentMaxSize + WM_HEADER_PARENT_MAX_SIZE +
                                    (ks ? WM_SIZE_KS_PARENT_DATA + WM_SIZE_MP_PARENT_PADDING : 0));
    sStatus->mp_maxRecvSize = (u16)(pp->childMaxSize + WM_HEADER_CHILD_MAX_SIZE +
                                    (ks ? WM_SIZE_KS_CHILD_DATA + WM_SIZE_MP_CHILD_PADDING : 0));
    sStatus->mp_parentSize = pp->parentMaxSize;
    sStatus->mp_childSize = pp->childMaxSize;
}

static void Attach(const u32 *cmd)
{
    sWm7 = (WMArm7Buf *)(uintptr_t)cmd[1];
    sStatus = (WMStatus *)(uintptr_t)cmd[2];
    if (sWm7 != NULL) {
        sWm7->status = sStatus;
        sWm7->fifo7to9 = (u32 *)(uintptr_t)cmd[3];
    }
    if (sStatus != NULL) {
        sStatus->miscFlags = cmd[4];
        sStatus->allowedChannel = NITRO_PORT_WM_CHANNELS;
        sStatus->enableChannel = NITRO_PORT_WM_CHANNELS;
        sStatus->linkLevel = WM_LINK_LEVEL_0;
        sStatus->aid = 0;
        sStatus->child_bitmap = 0;
        sStatus->mp_flag = FALSE;
        sStatus->pwrMgtMode = 0;
        OS_GetMacAddress(sStatus->MacAddress);
    }
}

static void SetState(u16 state)
{
    if (sStatus != NULL) {
        sStatus->state = state;
    }
}

static u16 State(void)
{
    return sStatus != NULL ? sStatus->state : WM_STATE_READY;
}

static void HandleCommand(const u32 *cmd)
{
    u16 apiid = (u16)(*(const u16 *)cmd & ~WM_API_REQUEST_ACCEPTED);

    switch (apiid) {
    case WM_APIID_INITIALIZE:
        Attach(cmd);
        SetState(WM_STATE_IDLE);
        Plain(apiid, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_ENABLE:
        Attach(cmd);
        SetState(WM_STATE_STOP);
        Plain(apiid, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_DISABLE:
        SetState(WM_STATE_READY);
        Plain(apiid, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_POWER_ON:
        SetState(WM_STATE_IDLE);
        Plain(apiid, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_POWER_OFF:
        SetState(WM_STATE_STOP);
        Plain(apiid, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_RESET:
        if (sStatus != NULL) {
            sStatus->mp_flag = FALSE;
            sStatus->child_bitmap = 0;
            sStatus->aid = 0;
        }
        SetState(WM_STATE_IDLE);
        Plain(apiid, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_END:
        /* (the library then lets go of its buffers, WM_Finish: no more
         * beacons from them) */
        SetState(WM_STATE_READY);
        Plain(apiid, WM_ERRCODE_SUCCESS);
        sStatus = NULL;
        sWm7 = NULL;
        break;
    case WM_APIID_SET_P_PARAM:
        if (sStatus != NULL) {
            memcpy(&sStatus->pparam, (const void *)(uintptr_t)cmd[1], sizeof(WMParentParam));
            SetMpSizes();
        }
        Plain(apiid, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_START_PARENT: {
        WMStartParentCallback *cb = NewReply(apiid, WM_ERRCODE_SUCCESS);

        SetState(WM_STATE_PARENT);
        if (sStatus != NULL) {
            sStatus->aid = 0;
            sStatus->child_bitmap = 0;
            cb->parentSize = sStatus->pparam.parentMaxSize;
            cb->childSize = sStatus->pparam.childMaxSize;
        }
        cb->state = WM_STATECODE_PARENT_START;
        Send(cb, 1);
        sNextBeacon = sVBlank + 1 + MsToVBlanks(sStatus && sStatus->pparam.beaconPeriod ? sStatus->pparam.beaconPeriod : 200);
        break;
    }
    case WM_APIID_END_PARENT:
        SetState(WM_STATE_IDLE);
        Plain(apiid, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_START_SCAN: {
        const WMStartScanReq *req = (const WMStartScanReq *)cmd;
        WMStartScanCallback *cb = NewReply(apiid, WM_ERRCODE_SUCCESS);

        SetState(WM_STATE_SCAN);
        cb->state = WM_STATECODE_PARENT_NOT_FOUND;
        cb->channel = req->channel;
        Send(cb, MsToVBlanks(req->maxChannelTime));
        break;
    }
    case WM_APIID_START_SCAN_EX: {
        const WMStartScanExReq *req = (const WMStartScanExReq *)cmd;
        WMStartScanExCallback *cb = NewReply(apiid, WM_ERRCODE_SUCCESS);
        u32 channels = 0, c;

        for (c = req->channelList; c; c &= c - 1) {
            channels++;
        }
        SetState(WM_STATE_SCAN);
        cb->state = WM_STATECODE_PARENT_NOT_FOUND;
        cb->channelList = req->channelList;
        cb->bssDescCount = 0;
        Send(cb, MsToVBlanks(req->maxChannelTime * (channels ? channels : 1)));
        break;
    }
    case WM_APIID_END_SCAN:
        SetState(WM_STATE_IDLE);
        Plain(apiid, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_START_CONNECT: {
        /* No parent is ever found, so there's none to connect to. */
        WMStartConnectCallback *cb = NewReply(apiid, WM_ERRCODE_FAILED);

        cb->state = WM_STATECODE_CONNECT_START;
        Send(cb, 1);
        break;
    }
    case WM_APIID_DISCONNECT: {
        WMDisconnectCallback *cb = NewReply(apiid, WM_ERRCODE_SUCCESS);

        cb->tryBitmap = (u16)cmd[1];
        cb->disconnectedBitmap = 0;
        Send(cb, 1);
        break;
    }
    case WM_APIID_START_MP: {
        const WMStartMPReq *req = (const WMStartMPReq *)cmd;
        WMStartMPCallback *cb = NewReply(apiid, WM_ERRCODE_SUCCESS);

        if (sStatus != NULL) {
            sStatus->mp_flag = TRUE;
            sStatus->mp_recvBufSize = (u16)req->recvBufSize;
            sStatus->mp_recvBuf[0] = (WMMpRecvBuf *)req->recvBuf;
            sStatus->mp_recvBuf[1] = (WMMpRecvBuf *)((u8 *)req->recvBuf + req->recvBufSize);
            sStatus->mp_sendBuf = req->sendBuf;
            sStatus->mp_sendBufSize = (u16)req->sendBufSize;
        }
        SetState(State() == WM_STATE_CHILD ? WM_STATE_MP_CHILD : WM_STATE_MP_PARENT);
        cb->state = WM_STATECODE_MP_START;
        Send(cb, 1);
        break;
    }
    case WM_APIID_END_MP:
        if (sStatus != NULL) {
            sStatus->mp_flag = FALSE;
        }
        SetState(State() == WM_STATE_MP_CHILD ? WM_STATE_CHILD : WM_STATE_PARENT);
        Plain(apiid, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_SET_MP_DATA: {
        /* (only with someone connected, which never happens: the library
         * itself refuses while no child is there) */
        WMPortSendCallback *cb = NewReply(WM_APIID_PORT_SEND, WM_ERRCODE_FAILED);

        cb->state = WM_STATECODE_PORT_SEND;
        cb->data = (const u16 *)(uintptr_t)cmd[1];
        cb->length = (u16)cmd[2];
        cb->destBitmap = (u16)cmd[3];
        cb->restBitmap = (u16)cmd[3];
        cb->port = (u16)cmd[4];
        cb->callback = (WMCallbackFunc)(uintptr_t)cmd[6];
        cb->arg = (void *)(uintptr_t)cmd[7];
        Send(cb, 1);
        break;
    }
    case WM_APIID_SET_GAMEINFO:
        if (sStatus != NULL) {
            sStatus->pparam.userGameInfo = (u16 *)(uintptr_t)cmd[1];
            sStatus->pparam.userGameInfoLength = (u16)cmd[2];
            sStatus->pparam.ggid = cmd[3];
            sStatus->pparam.tgid = (u16)cmd[4];
        }
        Plain(apiid, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_SET_ENTRY:
        if (sStatus != NULL) {
            sStatus->pparam.entryFlag = (u16)cmd[1];
        }
        Plain(apiid, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_SET_BEACON_PERIOD:
        if (sStatus != NULL) {
            sStatus->pparam.beaconPeriod = (u16)cmd[1];
        }
        Plain(apiid, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_MEASURE_CHANNEL: {
        const WMMeasureChannelReq *req = (const WMMeasureChannelReq *)cmd;
        WMMeasureChannelCallback *cb = NewReply(apiid, WM_ERRCODE_SUCCESS);

        cb->channel = req->channel;
        cb->ccaBusyRatio = 0;   /* nobody on any channel */
        Send(cb, MsToVBlanks(req->measureTime));
        break;
    }
    case WM_APIID_SET_LIFETIME:
    case WM_APIID_SET_BEACON_IND:
    case WM_APIID_SET_MP_PARAMETER:
    case WM_APIID_SET_WEPKEY:
    case WM_APIID_SET_WEPKEY_EX:
    case WM_APIID_SET_PS_MODE:
    case WM_APIID_AUTO_DEAUTH:
    case WM_APIID_AUTO_DISCONNECT:
    case WM_APIID_INIT_W_COUNTER:
    case WM_APIID_GET_W_COUNTER:
        Plain(apiid, WM_ERRCODE_SUCCESS);
        break;
    default:
        if (apiid < WM_APIID_ASYNC_KIND_MAX && !sReported[apiid]) {
            sReported[apiid] = TRUE;
            NitroPort_Log("[wm] request %u not played: answered as failed", apiid);
        }
        if (apiid < WM_APIID_ASYNC_KIND_MAX - 2) {
            Plain(apiid, WM_ERRCODE_FAILED);
        }
        break;
    }
}

void NitroPort_HandleWm(int tag, u32 data, BOOL err)
{
    u32 *cmd = (u32 *)(uintptr_t)data;
    (void)tag;

    if (err || cmd == NULL) {
        return;
    }
    HandleCommand(cmd);
    /* The command buffer is the ARM9's to use again. */
    *(u16 *)cmd |= WM_API_REQUEST_ACCEPTED;
}

void NitroPort_HandleWvr(int tag, u32 data, BOOL err)
{
    (void)tag;
    if (err) {
        return;
    }
    switch (data & 0xffff0000u) {
    case WVR_PXI_COMMAND_STARTUP:
        NitroPort_Log("[wm] the wireless driver started (a radio with nobody around)");
        Queue(PXI_FIFO_TAG_WVR, WVR_PXI_COMMAND_STARTUP | WVR_RESULT_SUCCESS, 2);
        break;
    case WVR_PXI_COMMAND_TERMINATE:
        Queue(PXI_FIFO_TAG_WVR, WVR_PXI_COMMAND_TERMINATE | WVR_RESULT_SUCCESS, 2);
        break;
    default:
        break;
    }
}

void NitroPort_ResetWmState(void)
{
    sQueueHead = 0;
    sQueueCount = 0;
    sNextBuf = 0;
    sWm7 = NULL;
    sStatus = NULL;
    memset(sReported, 0, sizeof(sReported));
}

/* Each V-blank (interrupts masked; they're taken when the V-blank's are):
 * the answers now due, and a parent's beacons. */
void NitroPort_WmVBlank(void)
{
    sVBlank++;
    if (sStatus != NULL && (sStatus->state == WM_STATE_PARENT || sStatus->state == WM_STATE_MP_PARENT) &&
        (s32)(sVBlank - sNextBeacon) >= 0) {
        WMStartParentCallback *cb = NewReply(WM_APIID_START_PARENT, WM_ERRCODE_SUCCESS);

        cb->state = WM_STATECODE_BEACON_SENT;
        cb->parentSize = sStatus->pparam.parentMaxSize;
        cb->childSize = sStatus->pparam.childMaxSize;
        Send(cb, 1);
        sNextBeacon = sVBlank + MsToVBlanks(sStatus->pparam.beaconPeriod ? sStatus->pparam.beaconPeriod : 200);
    }
    while (sQueueCount && (s32)(sVBlank - sQueue[sQueueHead].due) >= 0) {
        PendingReply p = sQueue[sQueueHead];

        sQueueHead = (sQueueHead + 1) % WM_REPLIES;
        sQueueCount--;
        NitroPort_Arm7Reply(p.tag, p.data, FALSE);
    }
}
