/* What ps2_ipu.c (from the Ace Combat 5 recompilation) takes from that runtime's headers, for this runtime.
 * Its log and fatal calls are renamed (ps2_log is a namespace here); ps2_ipu_glue.c implements them and the debug
 * hooks, ps2_memory.cpp the DMA side (ps2_dmac_ipu_*). */
#ifndef PS2X_IPU_COMPAT_H
#define PS2X_IPU_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#ifndef __cplusplus
/* Only in ps2_ipu.c (C): in C++ ps2_log is the runtime's log namespace. */
#define PS2_ENV(name) (getenv(name) != NULL)
#define ps2_log ps2x_ipu_log
#define ps2_fatal ps2x_ipu_fatal
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ps2_state_put)(void *ud, const char *tag, const void *p, u64 n);

void ps2x_ipu_log(const char *fmt, ...);
void ps2x_ipu_fatal(const char *fmt, ...);
void ps2_dump_trace(const char *why);
void ps2_gs_trx_history(void);

/* DMA side (ps2_memory.cpp): toIPU (channel 4) and fromIPU (channel 3). */
void ps2_dmac_ipu_pull(void);
void ps2_dmac_ipu_drain(void);
void ps2_dmac_ipu_report(u32 *chcr, u32 *madr, u32 *qwc, u32 *tadr);
void ps2_dmac_ipu_history(void);
void ps2_dmac_ipu_out_history(void);

/* ps2_ipu.c */
void ps2_ipu_init(void);
u32 ps2_ipu_read32(u32 addr);
void ps2_ipu_write32(u32 addr, u32 value);
void ps2_ipu_dma_in(const void *qw, u32 n);
void ps2_ipu_dma_out(void *dst, u32 n);
u32 ps2_ipu_in_space(void);
u32 ps2_ipu_out_avail(void);
u32 ps2_ipu_want(void);
u32 ps2_ipu_out_pos(void);
void ps2_ipu_report(void);
void ps2_ipu_state_save(ps2_state_put put, void *ud);

#ifdef __cplusplus
}
#endif

#endif
