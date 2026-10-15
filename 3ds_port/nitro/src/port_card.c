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

/* The save, as it is in the file, kept in memory: read once (one request
 * to the SD card), reads served from here, and each change written to the
 * file before the request completes. (Read and written in pieces of 256
 * bytes through stdio's 1 KB buffer, loading a save at Continue was some
 * 2000 requests to the card, 1.8 s, and saving 1.7 s; 2026-10-15.) */
static unsigned char sImage[SAVE_BYTES];

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
    /* The ARM9 must not observe request completion until data is durable
     * at the stdio level (including requests from async CARD APIs). */
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

static BOOL write_save(u32 offset, const void *src, u32 length, BOOL program)
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
    return store(offset, length);
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
    return store(offset, length);
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
