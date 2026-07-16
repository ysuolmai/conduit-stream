// AirPlay-1 (RAOP) receiver: RTSP control server. Public surface is deliberately
// tiny — main starts it after Wi-Fi GOT_IP (once esp_netif + the advertised
// _raop._tcp service exist). Single session at a time (spec §8: a second sender
// gets 453 Busy). The RTP audio path is Phase 3.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start the RTSP server task (listens on RAOP_RTSP_PORT = 5000). Safe to call
// once. Runs raop_crypto_init() internally on first start; if the crypto self-
// test fails the server is NOT started (we cannot negotiate without the key).
void raop_server_start(void);

// Stop the server task and tear down any live session (used on Wi-Fi LOST_IP later).
void raop_server_stop(void);

#ifdef __cplusplus
}
#endif
