/*
 * Virtual ARM7: sound, silent for now.
 *
 * On the DS the game's sound library (snd_command.c, ARM9 side) writes its
 * requests as lists of commands (start a sequence, set a volume ...) and
 * sends each list's address to the ARM7 over PXI. The ARM7's sound driver
 * runs them and then counts the list as finished in shared memory
 * (SNDSharedWork.finishCommandTag); only then does the ARM9 reuse those
 * commands. With nothing counting, the game runs out of its 256 commands
 * after a while and waits for the ARM7 forever: the first real-data run
 * stopped that way in the opening, ~30 seconds in.
 *
 * This service plays nothing. It takes each list, does the two things the
 * ARM9 itself reads back (where the shared memory is; the driver-info
 * snapshot, here "nothing is playing"), and counts the list as finished.
 * The shared memory's status words stay 0: no sequence, channel or capture
 * active, so code that waits for a sound to end goes on at once. Alarms
 * (timed callbacks the ARM7 would send back) never fire.
 *
 * Real sound (the ARM7 sequencer, mixed to the 3DS's speakers) replaces
 * this later.
 */
#include <nitro.h>
#include <nitro_port.h>
#include <nitro/snd/common/command.h>
#include <nitro/snd/common/work.h>

#include <stdint.h>
#include <string.h>

#define SND_MSG_REQUEST_COMMAND_PROC 0   /* snd_command.c: "process now" */

static SNDSharedWork *sShared;
static SNDWork sEmptyWork;               /* what the driver info points at */
static BOOL sToldSilent;

void NitroPort_ResetSoundState(void)
{
    sShared = NULL;
    memset(&sEmptyWork, 0, sizeof(sEmptyWork));
}

static void RunCommand(const SNDCommand *command)
{
    switch (command->id) {
    case SND_COMMAND_SHARED_WORK:
        sShared = (SNDSharedWork *)(uintptr_t)command->arg[0];
        break;
    case SND_COMMAND_READ_DRIVER_INFO: {
        SNDDriverInfo *info = (SNDDriverInfo *)(uintptr_t)command->arg[0];

        if (info != NULL) {
            memset(info, 0, sizeof(*info));
            info->workAddress = &sEmptyWork;
        }
        break;
    }
    case SND_COMMAND_START_SEQ:
    case SND_COMMAND_START_PREPARED_SEQ:
    case SND_COMMAND_SETUP_CHANNEL_PCM:
    case SND_COMMAND_SETUP_CHANNEL_PSG:
    case SND_COMMAND_SETUP_CHANNEL_NOISE:
    case SND_COMMAND_START_TIMER:
        if (!sToldSilent) {
            sToldSilent = TRUE;
            NitroPort_Log("[nitro] sound: requests are answered but nothing plays yet");
        }
        break;
    default:
        break;
    }
}

void NitroPort_HandleSound(int tag, u32 data, BOOL err)
{
    const SNDCommand *command;

    (void)tag;
    if (err || data == SND_MSG_REQUEST_COMMAND_PROC) {
        return;   /* lists are run as soon as they arrive */
    }
    for (command = (const SNDCommand *)(uintptr_t)data; command != NULL; command = command->next) {
        RunCommand(command);
    }
    if (sShared != NULL) {
        sShared->finishCommandTag++;
    }
}
