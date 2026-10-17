/* Log and debug hooks ps2_ipu.c calls (see ps2_ipu_compat.h). */
#include "ps2_ipu_compat.h"

#include <stdarg.h>
#include <stdio.h>

void ps2x_ipu_log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fputs("[ipu] ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

void ps2x_ipu_fatal(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fputs("[ipu] fatal: ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
    fflush(stderr);
    abort();
}

/* The AC5 runtime's trace dump and GS transfer history have no counterpart here. */
void ps2_dump_trace(const char *why)
{
    (void)why;
}

void ps2_gs_trx_history(void)
{
}
