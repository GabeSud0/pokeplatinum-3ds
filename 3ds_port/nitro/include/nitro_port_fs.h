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

/* NitroSDK ROM archive command processor (SDK_PORT_3DS only). */
FSResult NitroPort_PakArchiveProc(FSFile *file, FSCommandType command);

#ifdef __cplusplus
}
#endif
#endif
