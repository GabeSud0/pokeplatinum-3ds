/*
 * ARM7 CARD backup SPI service over the original NitroSDK PXI protocol.
 * 4 Mbit (512 KiB) FLASH is persisted as a raw .sav image. The ARM9 SDK
 * owns CARDiCommandArg and handles locks, paging, verification and callbacks;
 * we implement the ARM7's shared-memory command execution and ACK.
 */
#include <nitro.h>
#include <nitro_port.h>
#include "card_common.h"
#include "port_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAVE_BYTES (512u * 1024u)
#define ERASE_SECTOR_BYTES (64u * 1024u)
#define ERASE_SUBSECTOR_BYTES 4096u
#define ERASE_PAGE_BYTES 256u

static char *sSavePath;
static FILE *sSave;
static CARDiCommandArg *sCommand;
static BOOL sAwaitInitAddress;

/* The save's lock: a card request, or NitroPort_CardFlush from the HOME
 * menu's or closing's thread. */
static volatile char sLock;

static void lock(void)
{
    while (__atomic_test_and_set(&sLock, __ATOMIC_ACQUIRE)) {
        NitroPortPlatform_Sleep(100);
    }
}

static BOOL try_lock(uint32_t ms)
{
    while (__atomic_test_and_set(&sLock, __ATOMIC_ACQUIRE)) {
        if (ms-- == 0) {
            return FALSE;
        }
        NitroPortPlatform_Sleep(1000);
    }
    return TRUE;
}

static void unlock(void)
{
    __atomic_clear(&sLock, __ATOMIC_RELEASE);
}

static void flush_locked(void);

void NitroPort_SetSavePath(const char *path)
{
    char *copy = NULL;
    if (path && *path) {
        size_t len = strlen(path);
        copy = (char *)malloc(len + 1);
        if (!copy) {
            NitroPort_Log("[ARM7 CARD] cannot allocate save path");
            return;
        }
        memcpy(copy, path, len + 1);
    }
    lock();
    flush_locked();   /* (the old save's last changes into its file) */
    if (sSave) {
        fclose(sSave);
        sSave = NULL;
    }
    free(sSavePath);
    sSavePath = copy;
    unlock();
}

/* The save, as it is in the file, kept in memory: read once (one request
 * to the SD card), reads served from here, and each change written to the
 * file before the request that ends it completes. (Read and written in
 * pieces of 256 bytes through stdio's 1 KB buffer, loading a save at
 * Continue was some 2000 requests to the card, 1.8 s, and saving 1.7 s;
 * 2026-10-15.)
 *
 * The SDK writes a save 256 bytes (a page) a request: a full save was 512
 * writes to the SD card, 3.3 s on the 3DS with the game held up all that
 * time (2026-10-16). The pages of one write (CARD_WriteAndVerifyFlash...:
 * the game's block of the save, then each footer) go into the file
 * together, with its last page: what the game waits for (its callback,
 * its result) is the whole write in the file, as before; a write cut
 * short (the 3DS turned off) leaves the file as it was, the save's other
 * copy whole, as a write cut short on the DS's flash would. */
static unsigned char sImage[SAVE_BYTES];
static u32 sPendingLo = SAVE_BYTES, sPendingHi;   /* changed, not in the file yet */
static u32 sFileWrites;

static void close_save(void)
{
    if (sSave) {
        fclose(sSave);
        sSave = NULL;
    }
}

static BOOL open_save(void)
{
    long size;
    if (sSave) return TRUE;
    if (!sSavePath) return FALSE;
    sSave = fopen(sSavePath, "r+b");
    if (!sSave) {
        sSave = fopen(sSavePath, "w+b");
        if (!sSave) return FALSE;
        (void)setvbuf(sSave, NULL, _IONBF, 0);
        memset(sImage, 0xff, sizeof(sImage));
        if (fwrite(sImage, 1, SAVE_BYTES, sSave) != SAVE_BYTES || fflush(sSave) != 0)
            goto invalid;
    } else {
        (void)setvbuf(sSave, NULL, _IONBF, 0);
    }
    if (fseek(sSave, 0, SEEK_END) != 0 ||
        (size = ftell(sSave)) != (long)SAVE_BYTES) goto invalid;
    if (fseek(sSave, 0, SEEK_SET) != 0 ||
        fread(sImage, 1, SAVE_BYTES, sSave) != SAVE_BYTES) goto invalid;
    return TRUE;
invalid:
    close_save();
    return FALSE;
}

static BOOL range_ok(u32 offset, u32 length)
{
    return offset <= SAVE_BYTES && length <= SAVE_BYTES - offset;
}

static BOOL read_save(u32 offset, void *dst, u32 length)
{
    if (!range_ok(offset, length) || (length && !dst) || !open_save())
        return FALSE;
    memcpy(dst, sImage + offset, length);
    return TRUE;
}

/* The image's bytes offset .. offset + length into the file. If that
 * fails, the image is read back from the file there (it stays what the
 * file holds), and the request fails. */
static BOOL store(u32 offset, u32 length)
{
    /* The ARM9 must not see a write complete before it's in the file at
     * the stdio level (async CARD APIs included): this is its last page's
     * request, the write's earlier pages with it (flush_pending). */
    if (fseek(sSave, (long)offset, SEEK_SET) == 0 &&
        fwrite(sImage + offset, 1, length, sSave) == length && fflush(sSave) == 0)
        return TRUE;
    if (fseek(sSave, (long)offset, SEEK_SET) != 0 ||
        fread(sImage + offset, 1, length, sSave) != length) {
        /* (can't tell what the file has: read it all again next time) */
        close_save();
    }
    return FALSE;
}

/* What's changed and not in the file yet, into it (one write): FALSE if
 * that failed (the image is the file's again there). */
static BOOL flush_pending(void)
{
    u32 lo = sPendingLo, hi = sPendingHi;
    u64 start;
    BOOL ok;

    if (lo >= hi) {
        return TRUE;
    }
    sPendingLo = SAVE_BYTES;
    sPendingHi = 0;
    if (!open_save()) {
        return FALSE;
    }
    start = NitroPortPlatform_Nanoseconds();
    ok = store(lo, hi - lo);
    sFileWrites++;
    if (hi - lo >= 4096) {
        NitroPort_Log("[save] %u KB into the file in %.0f ms%s", (unsigned)((hi - lo) / 1024),
                      (double)(NitroPortPlatform_Nanoseconds() - start) / 1e6, ok ? "" : ": it FAILED");
    }
    return ok;
}

static void flush_locked(void)
{
    if (sPendingLo < sPendingHi && sSave != NULL) {
        (void)flush_pending();
    }
}

static void note_pending(u32 offset, u32 length)
{
    if (offset < sPendingLo) {
        sPendingLo = offset;
    }
    if (offset + length > sPendingHi) {
        sPendingHi = offset + length;
    }
}

/* later: a page of a longer write, not its last (into the file with that) */
static BOOL write_save(u32 offset, const void *src, u32 length, BOOL program, BOOL later)
{
    const unsigned char *data = (const unsigned char *)src;
    u32 i;
    if (!range_ok(offset, length) || (length && !src) || !open_save())
        return FALSE;
    if (program) {
        for (i = 0; i < length; ++i)
            sImage[offset + i] &= data[i];
    } else {
        memcpy(sImage + offset, data, length);
    }
    note_pending(offset, length);
    return later ? TRUE : flush_pending();
}

static BOOL verify_save(u32 offset, const void *source, u32 length)
{
    if (!range_ok(offset, length) || (length && !source) || !open_save()) return FALSE;
    return memcmp(sImage + offset, source, length) == 0 ? TRUE : FALSE;
}

static BOOL erase_save(u32 offset, u32 length)
{
    if (!range_ok(offset, length) || !open_save()) return FALSE;
    memset(sImage + offset, 0xff, length);
    note_pending(offset, length);
    return flush_pending();
}

static CARDResult execute_card_command(CARDRequest request)
{
    CARDiCommandArg *cmd = sCommand;
    BOOL okay = FALSE, later = FALSE;
    if (!cmd) return CARD_RESULT_NO_RESPONSE;
    if (request == CARD_REQ_WRITE_BACKUP) {
        /* A page of a longer write (CARDi_RequestStreamCommandCore, the
         * only one that asks for WRITE: the ARM9's task, waiting for this
         * request, has the rest of it still to go, this page included),
         * not its last: into the file with the last. */
        later = cardi_common.len > cmd->len && cardi_common.dst == cmd->dst;
    } else if (request != CARD_REQ_VERIFY_BACKUP && sSave != NULL &&
               sPendingLo < sPendingHi && !flush_pending()) {
        /* (anything else: what's changed into the file first) */
        return CARD_RESULT_FAILURE;
    }
    if (request == CARD_REQ_IDENTIFY) {
        /* Pokémon Platinum uses the 4 Mbit FLASH cartridge. Reject other
         * sizes so we can never silently corrupt a 512 KiB .sav image. */
        return cmd->type == CARD_BACKUP_TYPE_FLASH_4MBITS && open_save()
                   ? CARD_RESULT_SUCCESS : CARD_RESULT_UNSUPPORTED;
    }
    if (cmd->type != CARD_BACKUP_TYPE_FLASH_4MBITS)
        return CARD_RESULT_UNSUPPORTED;

    switch (request) {
    case CARD_REQ_READ_BACKUP:
        okay = read_save(cmd->src, (void *)(uintptr_t)cmd->dst, cmd->len);
        break;
    case CARD_REQ_WRITE_BACKUP:
        okay = write_save(cmd->dst, (const void *)(uintptr_t)cmd->src,
                          cmd->len, FALSE, later);
        break;
    case CARD_REQ_PROGRAM_BACKUP:
        okay = write_save(cmd->dst, (const void *)(uintptr_t)cmd->src,
                          cmd->len, TRUE, FALSE);
        break;
    case CARD_REQ_VERIFY_BACKUP:
        okay = verify_save(cmd->dst, (const void *)(uintptr_t)cmd->src, cmd->len);
        break;
    case CARD_REQ_ERASE_PAGE_BACKUP:
        okay = cmd->len <= ERASE_PAGE_BYTES && erase_save(cmd->dst, cmd->len);
        break;
    case CARD_REQ_ERASE_SECTOR_BACKUP:
        okay = cmd->len <= ERASE_SECTOR_BYTES && erase_save(cmd->dst, cmd->len);
        break;
    case CARD_REQ_ERASE_SUBSECTOR_BACKUP:
        okay = cmd->len <= ERASE_SUBSECTOR_BYTES && erase_save(cmd->dst, cmd->len);
        break;
    case CARD_REQ_ERASE_CHIP_BACKUP:
        okay = erase_save(0, SAVE_BYTES);
        break;
    case CARD_REQ_READ_STATUS:
        if (cmd->dst) {
            *(u8 *)(uintptr_t)cmd->dst = 0; /* WIP=0: no pending program */
            okay = TRUE;
        }
        break;
    case CARD_REQ_WRITE_STATUS:
        okay = TRUE;
        break;
    default:
        return CARD_RESULT_UNSUPPORTED;
    }
    return okay ? CARD_RESULT_SUCCESS : CARD_RESULT_FAILURE;
}

void NitroPort_HandleCard(int tag, u32 data, BOOL err)
{
    CARDRequest req;
    (void)tag;
    if (!err) return; /* CARD messages set the PXI error/type bit. */
    if (sAwaitInitAddress) {
        sAwaitInitAddress = FALSE;
        /* Use the actual ARM9-owned command block, never an untrusted
         * pointer reconstructed from a PXI word on a 64-bit host. */
        if (data == (u32)(uintptr_t)cardi_common.cmd)
            sCommand = cardi_common.cmd;
        if (sCommand) sCommand->result = CARD_RESULT_SUCCESS;
        NitroPort_Arm7Reply(PXI_FIFO_TAG_FS, CARD_REQ_ACK, TRUE);
        return;
    }
    req = (CARDRequest)data;
    if (req == CARD_REQ_INIT) {
        sAwaitInitAddress = TRUE;
        return; /* ACK after ARM9 sends its shared-memory address. */
    }
    if (!sCommand) sCommand = cardi_common.cmd;
    if (sCommand) {
        lock();
        sCommand->result = execute_card_command(req);
        unlock();
    }
    NitroPort_Arm7Reply(PXI_FIFO_TAG_FS, CARD_REQ_ACK, TRUE);
}

void NitroPort_CardFlush(void)
{
    /* (a request under way: its last page writes it all, soon) */
    if (try_lock(200)) {
        flush_locked();
        unlock();
    }
}

uint32_t NitroPort_CardFileWrites(void)
{
    return sFileWrites;
}
