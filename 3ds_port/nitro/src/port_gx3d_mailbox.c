#include <nitro_port_gx3d_mailbox.h>
#include "port_platform.h"
#include <stddef.h>
/* Core-0 producer outranks the GPU worker on Old 3DS. An endless spin
 * could starve the owner of this gate forever. Mirror the audio lock:
 * spin briefly, then yield CPU time even to a lower-priority owner. */
static void lock(NitroGx3dMailbox *b) {
    unsigned spins = 0;
    while (__atomic_test_and_set(&b->gate, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&b->gate, __ATOMIC_RELAXED)) {
            if (++spins >= 64u) {
                NitroPortPlatform_Sleep(50);
                spins = 0;
            }
        }
    }
}
static void unlock(NitroGx3dMailbox *b) {
    __atomic_clear(&b->gate, __ATOMIC_RELEASE);
}
void NitroPort_MailboxInit(NitroGx3dMailbox *b) {
    if(!b)return;
    b->gate=0; b->building=0; b->latest=-1; b->reading=-1;
    b->serial=0; b->skipped=0;
}
int NitroPort_MailboxPublish(NitroGx3dMailbox *b, int *next, uint32_t *serial) {
    int i,freeSlot=-1;
    if(!b)return 0;
    lock(b);
    /* Only three slots exist. If the consumer holds the latest and the
     * producer has built the third, discard this frame, not the reader. */
    for(i=0;i<3;i++)if(i!=b->building && i!=b->reading){
        freeSlot=i;break;
    }
    if(freeSlot<0){
        b->skipped++;
        unlock(b);
        return 0;
    }
    b->latest=b->building;
    b->building=freeSlot;
    b->serial++;
    if(next)*next=freeSlot;
    if(serial)*serial=b->serial;
    unlock(b);
    return 1;
}
int NitroPort_MailboxAcquire(NitroGx3dMailbox *b,uint32_t *lastSerial) {
    int slot=-1;
    if(!b||!lastSerial)return -1;
    lock(b);
    if(b->reading<0 && b->latest>=0 && b->serial!=*lastSerial){
        b->reading=slot=b->latest;
        *lastSerial=b->serial;
    }
    unlock(b);
    return slot;
}
void NitroPort_MailboxRelease(NitroGx3dMailbox *b,int index) {
    if(!b)return;
    lock(b);
    if(index>=0 && index==b->reading)b->reading=-1;
    unlock(b);
}
int NitroPort_MailboxLatest(NitroGx3dMailbox *b) {
    int slot;
    if(!b)return -1;
    lock(b);
    slot=b->latest;
    unlock(b);
    return slot;
}
