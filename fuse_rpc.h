/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PYRO_FUSE_RPC_H
#define PYRO_FUSE_RPC_H
#ifdef __cplusplus
extern "C" {
#endif
extern int pyro_rpc_enabled;
int pyro_rpc_arguments(int *argc, char **argv);
int pyro_rpc_start(void);
int pyro_rpc_boundary(void);
void pyro_rpc_advanced(void);
#ifdef __cplusplus
}
#endif
#endif
