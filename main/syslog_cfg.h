#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Where the remote syslog client sends, and what it puts in the header. The
// sender itself is syslog.c; this module only stores and validates, so the
// console and the web handler can check a candidate configuration without
// caring whether the sender is running.

// 32 usable characters. RFC 5424 allows 255 for HOSTNAME, but this number sizes
// the datagram budget in syslog.c (SYSLOG_DGRAM_MAX), and 255 would push the
// worst case past the 480 bytes RFC 5426 guarantees every receiver accepts.
// Nothing needs a 255-character name for a device on one /24.
#define SYSLOG_CFG_HOSTNAME_MAX_LEN 33   // includes NUL

#define SYSLOG_CFG_DEFAULT_PORT     514
#define SYSLOG_CFG_DEFAULT_FACILITY 23   // local7, what routers conventionally use
#define SYSLOG_CFG_DEFAULT_SEVERITY 7    // debug: send everything the ring holds

typedef struct {
    bool     enabled;
    uint32_t server_ip;     // network byte order, like esp_ip4_addr_t.addr
    uint16_t port;
    uint8_t  facility;      // 0-23
    uint8_t  min_severity;  // 0-7; a line is sent when its severity is <= this
    char     hostname[SYSLOG_CFG_HOSTNAME_MAX_LEN];
} syslog_cfg_t;

// Must be called once (scheduler running, before any web or console access) to
// set up the RAM cache, for the same reason auth_cfg_init() must be.
esp_err_t syslog_cfg_init(void);

// Current configuration, from a RAM cache populated from NVS on first use. An
// empty stored hostname is resolved to the MAC-derived default here, so no
// caller has to know that "unset" and "the default" are the same thing.
void syslog_cfg_get(syslog_cfg_t *out);

// Persists a configuration and updates the cache. Bumps the generation counter
// below on success, whether or not anything actually changed - "saved" is the
// event the sender needs to hear about, and comparing structures to suppress a
// no-op save would only mean the sender occasionally re-reads what it already
// had.
esp_err_t syslog_cfg_save(const syslog_cfg_t *cfg);

// Incremented by every successful save. This is what tells the sender its
// configuration moved, and it is the authoritative mechanism: syslog.c checks
// it on every wake-up including its idle tick, so a caller that forgets to call
// syslog_config_changed() costs a second of latency rather than correctness.
uint32_t syslog_cfg_generation(void);

// Validates a candidate configuration. `subnet_ip`/`subnet_mask` are the
// bridge's own address and mask (syslog_get_subnet() supplies them); passing
// 0/0 means "could not read them", which is rejected rather than skipped - see
// the comment in the implementation. Returns true and leaves *err_msg untouched
// on success; returns false and sets *err_msg to a static reason on failure.
bool syslog_cfg_validate(const syslog_cfg_t *cfg, uint32_t subnet_ip,
                         uint32_t subnet_mask, const char **err_msg);

// The default HOSTNAME field: "wt32-bridge-" plus the low three bytes of the
// Ethernet MAC. Buffer should be SYSLOG_CFG_HOSTNAME_MAX_LEN bytes.
void syslog_cfg_default_hostname(char *out, size_t outsz);

// Name for a facility number ("local7", "daemon", ...), for the console and the
// web UI. Returns "local7"-style names for the ones that have them and a
// decimal string for the rest, so it never returns NULL.
const char *syslog_cfg_facility_name(uint8_t facility);

// Name for a severity number ("error", "warning", ...). Deliberately the same
// vocabulary the "loglevel" command and the log page's capture select use where
// the two overlap, so the interfaces do not disagree about what "warning"
// means. Returns NULL for an out-of-range value.
const char *syslog_cfg_severity_name(uint8_t severity);

// Parses a severity name back to its number. Returns false if unrecognised.
bool syslog_cfg_severity_from_name(const char *name, uint8_t *out);

#ifdef __cplusplus
}
#endif
