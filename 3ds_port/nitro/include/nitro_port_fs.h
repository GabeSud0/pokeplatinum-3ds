/*
 * ROM archive backed by the player's locally generated Platinum data pack.
 * No copyrighted game bytes are included in the program or repository.
 */
#ifndef NITRO_PORT_FS_H_
#define NITRO_PORT_FS_H_

#include <nitro/fs.h>
#include <pak.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Call after mounting SD (on 3DS), before game file access.
 * manifest_path is the extracted builder/manifest.json, copied alongside
 * the .pak to make filename -> original NitroFS ID lookup possible. v1
 * .pak deliberately contains IDs, not filenames.
 *
 * Call NitroPort_ClosePak only after all FSFile handles have been closed.
 * The same hook accepts regular host filesystem paths in PC tests.
 */
PakResult NitroPort_OpenPak(const char *pak_path, const char *manifest_path,
                            const char *expected_build_id);
void NitroPort_ClosePak(void);
BOOL NitroPort_IsPakOpen(void);
/* The port's own files in the pack (docs/PAK_FORMAT.md): a size (0: none),
 * and a read of [offset, offset + size). The game's thread. */
uint32_t NitroPort_PakOwnFileSize(uint32_t id);
BOOL NitroPort_PakOwnFileRead(uint32_t id, uint32_t offset, void *dest, uint32_t size);

/* Called when the game asks for a path the pack doesn't have. Weak; the
 * default logs it. */
void NitroPort_FileMissing(const char *path);

/* The game's file reads since the start (for the log: a long frame's
 * reads): how many, their bytes, and the time they took in microseconds. */
void NitroPort_FsReadStats(uint32_t *reads, uint64_t *bytes, uint64_t *micros);
/* Of those, the reads of the pack file itself (not from what's kept in
 * memory of it): how many, their bytes. */
void NitroPort_PakDiskStats(uint32_t *reads, uint64_t *bytes);

/* More of the pack kept in memory (8 KB blocks, 2048 at most: a New
 * 3DS's 16 MB), and the small files the field reads at every turn read
 * into it now (NitroPort_PakWarmField: before the game starts, so a
 * route's first crossing doesn't go to the SD card for them). The
 * kilobytes read. */
void NitroPort_PakCacheBlocks(uint32_t blocks);
uint32_t NitroPort_PakWarmField(void);

/* NitroSDK ROM archive command processor (SDK_PORT_3DS only). */
FSResult NitroPort_PakArchiveProc(FSFile *file, FSCommandType command);

#ifdef __cplusplus
}
#endif
#endif
