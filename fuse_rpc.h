/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PYRO_FUSE_RPC_H
#define PYRO_FUSE_RPC_H
#ifdef __cplusplus
extern "C" {
#endif
extern int pyro_rpc_enabled;
extern int pyro_rpc_trace_enabled;
extern int pyro_rpc_audio_enabled;
/* Opt-in replay ownership: host focus/keyboard events must not alter RPC keys. */
int pyro_rpc_owns_keyboard(void);
void pyro_rpc_audio_ula(unsigned at_tstates, int mic_on, int beeper_on);
void pyro_rpc_audio_pcm(const short *samples, unsigned count, unsigned channels);
int pyro_rpc_arguments(int *argc, char **argv);
int pyro_rpc_start(void);
int pyro_rpc_boundary(void);
void pyro_rpc_advanced(void);
void pyro_rpc_memory_write(unsigned address, unsigned before, unsigned value);
void pyro_rpc_record_contention(unsigned cycles);
#ifdef CORETEST
#define pyro_rpc_contention(cycles) (cycles)
#else
static inline unsigned pyro_rpc_contention(unsigned cycles) {
    if (pyro_rpc_trace_enabled) pyro_rpc_record_contention(cycles);
    return cycles;
}
#endif
#ifdef __cplusplus
}
#endif
#endif
