#ifndef DASHCDG_NET_QOS_H
#define DASHCDG_NET_QOS_H

/*
 * IPv4 DSCP (upper 6 bits of IP TOS) for dashcdg UDP: multicast media, stats, repair, PTP, etc.
 *
 * Espressif Wi‑Fi (AMPDU / block ack): the driver tracks Block Ack sessions per precedence / WMM
 * queue. Using many different DSCP values (hence many precedences) across concurrent sockets
 * increases internal RAM; prefer a single default for all dashcdg traffic on a host.
 *
 * WMM mapping (typical): DSCP EF (46) is often steered to AC_VO (voice), which does not use AMPDU
 * on ESP32. DSCP AF41 (34) is commonly mapped to AC_VI (video), which does support AMPDU along
 * with AC_BE and AC_BK. We default to AF41 so TX/RX can aggregate when AMPDU is enabled.
 *
 * Override for experiments: set `DASHCDG_NET_DSCP_DASHCDG_DEFAULT` before including headers (not
 * supported on all build graphs) or change this file / use CS0 (0) for pure best-effort.
 */
#define DASHCDG_NET_DSCP_EF 46U
#define DASHCDG_NET_DSCP_AF41 34U
#define DASHCDG_NET_DSCP_DASHCDG_DEFAULT DASHCDG_NET_DSCP_AF41

#endif
