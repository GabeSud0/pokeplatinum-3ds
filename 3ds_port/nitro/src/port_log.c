/*
 * Logging and "not implemented yet" reports for the port.
 */

#include <nitro_port.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MAX_REPORTED 512

static const char *sReported[MAX_REPORTED];
static int sReportedCount;

__attribute__((weak)) void NitroPort_Log(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

BOOL NitroPort_WasReported(const char *what)
{
    int i;

    for (i = 0; i < sReportedCount; i++) {
        if (strcmp(sReported[i], what) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

int NitroPort_Unimplemented(const char *what)
{
    if (!NitroPort_WasReported(what) && sReportedCount < MAX_REPORTED) {
        sReported[sReportedCount++] = what; /* callers pass string literals */
        NitroPort_Log("[nitro] not implemented: %s", what);
    }
    return sReportedCount;
}
