/*
 * ARM7 CARD backup SPI service over the original NitroSDK PXI protocol.
 * 4 Mbit (512 KiB) FLASH is persisted as a raw .sav image. The ARM9 SDK
 * owns CARDiCommandArg and handles locks, paging, verification and callbacks;
 * we implement the ARM7's shared-memory command execution and ACK.
 */
#include <nitro.h>
#include <nitro_port.h>
#include "card_common.h"

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
    if (sSave) {
        fclose(sSave);
        sSave = NULL;
    }
    free(sSavePath);
    sSavePath = copy;
}

static BOOL open_save(void)
{
    long size;
    if (sSave) return TRUE;
    if (!sSavePath) return FALSE;
    sSave = fopen(sSavePath, "r+b");
    if (!sSave) {
        unsigned char erased[4096];
        size_t written = 0;
        sSave = fopen(sSavePath, "w+b");
        if (!sSave) return FALSE;
        memset(erased, 0xff, sizeof(erased));
        while (written < SAVE_BYTES) {
            if (fwrite(erased, 1, sizeof(erased), sSave) != sizeof(erased))
                goto invalid;
            written += sizeof(erased);
        }
        if (fflush(sSave) != 0) goto invalid;
    }
    if (fseek(sSave, 0, SEEK_END) != 0 ||
        (size = ftell(sSave)) != (long)SAVE_BYTES) goto invalid;
    return TRUE;
invalid:
    fclose(sSave);
    sSave = NULL;
    return FALSE;
}

static BOOL range_ok(u32 offset, u32 length)
{
    return offset <= SAVE_BYTES && length <= SAVE_BYTES - offset;
}

static BOOL read_save(u32 offset, void *dst, u32 length)
{
    if (!range_ok(offset, length) || (length && !dst) ||
        !open_save() || fseek(sSave, (long)offset, SEEK_SET) != 0)
        return FALSE;
    return fread(dst, 1, length, sSave) == length ? TRUE : FALSE;
}

static BOOL write_save(u32 offset, const void *src, u32 length, BOOL program)
{
    const unsigned char *data = (const unsigned char *)src;
    unsigned char original[256];
    unsigned char masked[256];
    u32 processed = 0;
    if (!range_ok(offset, length) || (length && !src) || !open_save())
        return FALSE;
    while (processed < length) {
        u32 n = length - processed;
        u32 i;
        if (n > sizeof(masked)) n = sizeof(masked);
        if (program) {
            if (!read_save(offset + processed, original, n)) return FALSE;
            for (i = 0; i < n; ++i)
                masked[i] = original[i] & data[processed + i];
        }
        if (fseek(sSave, (long)(offset + processed), SEEK_SET) != 0 ||
            fwrite(program ? masked : data + processed, 1, n, sSave) != n)
            return FALSE;
        processed += n;
    }
    /* The ARM9 must not observe request completion until data is durable
     * at the stdio level (including requests from async CARD APIs). */
    return fflush(sSave) == 0 ? TRUE : FALSE;
}

static BOOL verify_save(u32 offset, const void *source, u32 length)
{
    const unsigned char *data = (const unsigned char *)source;
    unsigned char block[256];
    u32 processed = 0;
    if (!range_ok(offset, length) || (length && !source)) return FALSE;
    while (processed < length) {
        u32 n = length - processed;
        if (n > sizeof(block)) n = sizeof(block);
        if (!read_save(offset + processed, block, n) ||
            memcmp(data + processed, block, n) != 0)
            return FALSE;
        processed += n;
    }
    return TRUE;
}

static BOOL erase_save(u32 offset, u32 length)
{
    unsigned char bytes[4096];
    u32 written = 0;
    if (!range_ok(offset, length) || !open_save()) return FALSE;
    memset(bytes, 0xff, sizeof(bytes));
    if (fseek(sSave, (long)offset, SEEK_SET) != 0) return FALSE;
    while (written < length) {
        u32 n = length - written;
        if (n > sizeof(bytes)) n = sizeof(bytes);
        if (fwrite(bytes, 1, n, sSave) != n) return FALSE;
        written += n;
    }
    return fflush(sSave) == 0 ? TRUE : FALSE;
}

static CARDResult execute_card_command(CARDRequest request)
{
    CARDiCommandArg *cmd = sCommand;
    BOOL okay = FALSE;
    if (!cmd) return CARD_RESULT_NO_RESPONSE;
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
                          cmd->len, FALSE);
        break;
    case CARD_REQ_PROGRAM_BACKUP:
        okay = write_save(cmd->dst, (const void *)(uintptr_t)cmd->src,
                          cmd->len, TRUE);
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
    if (sCommand) sCommand->result = execute_card_command(req);
    NitroPort_Arm7Reply(PXI_FIFO_TAG_FS, CARD_REQ_ACK, TRUE);
}
