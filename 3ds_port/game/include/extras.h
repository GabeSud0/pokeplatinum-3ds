/*
 * extras.h is a header of Metrowerks' C library (MSL) with non-standard
 * string helpers. NitroDWC's GameSpy headers include it on the DS
 * (gs/nonport.h, under _NITRO); GCC's C libraries don't have it. Nothing the
 * game's sources use from it goes beyond the standard string functions, so
 * this stands in for it. The headers also expect time_t, which Metrowerks'
 * headers provided along the way (glibc does too; newlib, devkitARM's C
 * library, doesn't). Networking itself is not ported.
 */
#ifndef PORT_MSL_EXTRAS_H
#define PORT_MSL_EXTRAS_H

#include <string.h>
#include <time.h>

#endif
