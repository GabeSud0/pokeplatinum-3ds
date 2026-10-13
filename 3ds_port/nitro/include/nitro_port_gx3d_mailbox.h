/* A three-slot, non-blocking scene handoff. Plain C; no SDK/3DS headers.
 * Exactly one producer and one consumer; one reader can hold a slot while
 * SWAP publishes newer scenes. All index changes are protected by gate.
 * The caller owns all three scene allocations: no 600-KB scene copies. */
#ifndef NITRO_PORT_GX3D_MAILBOX_H
#define NITRO_PORT_GX3D_MAILBOX_H
#include <stdint.h>
typedef struct NitroGx3dMailbox {
    unsigned char gate;
    int building, latest, reading;
    uint32_t serial, skipped;
    uint32_t taken;     /* the serial of the scene last acquired */
    uint32_t tag[3];    /* the producer's number for each slot's scene */
} NitroGx3dMailbox;
void NitroPort_MailboxInit(NitroGx3dMailbox *box);
/* Publish the building slot; return 1 or 0 when no free building slot.
 * On success, returns the NEW building index in *next. On rejection the
 * same building slot can be cleared/reused; never wait for the GPU. */
int NitroPort_MailboxPublish(NitroGx3dMailbox *box, int *next, uint32_t *serial);
/* Returns -1 if no newer scene (or one is already acquired). */
int NitroPort_MailboxAcquire(NitroGx3dMailbox *box, uint32_t *lastSerial);
void NitroPort_MailboxRelease(NitroGx3dMailbox *box, int index);
int NitroPort_MailboxLatest(NitroGx3dMailbox *box);
/* Number the scene being built (before publishing it). */
void NitroPort_MailboxTag(NitroGx3dMailbox *box, uint32_t tag);
/* The lowest number of a scene still to be read: the one acquired, or the
 * latest if it hasn't been. 0 if neither. */
uint32_t NitroPort_MailboxUnread(NitroGx3dMailbox *box);
/* The latest scene was read without acquiring it (single-threaded use). */
void NitroPort_MailboxMarkTaken(NitroGx3dMailbox *box);
#endif
