#pragma once

#include "config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 0 on success. Sockets and the signalling task stay down when this fails. */
int net_start(const ic_config_t *cfg);

/* Join the saved network. Starts the stack the first time, otherwise
 * only changes the association. Call from the UI task. */
int net_join_wifi(const ic_config_t *cfg);

/* Apply a deferred Wi-Fi link edge. Call from the UI task only. */
void net_apply_link(void);

/* 0 when the station has no association. Otherwise the RSSI in dBm. */
int net_rssi_dbm(void);

#ifdef __cplusplus
}
#endif
