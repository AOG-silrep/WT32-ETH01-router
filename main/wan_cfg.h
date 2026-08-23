#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Which upstream network WiFi as WAN joins, and which destination ports a
// LAN client is allowed to reach through it. The WAN itself is wan.c; this
// module only stores and validates, so the console and the web handler can
// check a candidate configuration without caring whether the WAN is running
// - the same split, and the same reason, as syslog_cfg.c and syslog.c.

#define WAN_CFG_SSID_MAX_LEN     33   // includes NUL; 802.11 SSID is 32 octets
#define WAN_CFG_PASSWORD_MAX_LEN 64   // includes NUL; WPA2-PSK is 8-63

// Twelve is not a routing limit, it is a policy one. The point of this feature
// is that a tablet on a metered hotspot cannot reach anything it was not sent
// there to reach, and a list long enough to need scrolling is a list nobody
// audits. Twelve rather than eight because the defaults now spend six of them:
// RustDesk alone needs four rules, one of which is the same port twice.
#define WAN_CFG_MAX_PORTS 12

// Longest a formatted list can be: "65535/udp," is 10 characters per rule, and
// the last separator is replaced by the NUL.
#define WAN_CFG_PORTS_STR_MAX (10 * WAN_CFG_MAX_PORTS + 1)

typedef enum {
    WAN_PROTO_TCP = 0,   // zero so a rule read from an older blob is TCP
    WAN_PROTO_UDP = 1,
} wan_proto_t;

typedef struct {
    uint16_t port;   // host order
    uint8_t  proto;  // wan_proto_t
} wan_port_rule_t;

typedef struct {
    bool            enabled;
    char            ssid[WAN_CFG_SSID_MAX_LEN];
    char            password[WAN_CFG_PASSWORD_MAX_LEN];  // empty means an open network
    wan_port_rule_t ports[WAN_CFG_MAX_PORTS];            // allowed destinations
    uint8_t         nports;
} wan_cfg_t;

// Must be called once (scheduler running, before any web or console access) to
// set up the RAM cache, for the same reason auth_cfg_init() and
// syslog_cfg_init() must be.
esp_err_t wan_cfg_init(void);

// Current configuration, from a RAM cache populated from NVS on first use.
void wan_cfg_get(wan_cfg_t *out);

// Persists a configuration and updates the cache. Bumps the generation counter
// below on success whether or not anything changed, for the reason
// syslog_cfg_save() records: "saved" is the event a consumer needs to hear
// about, and suppressing a no-op save only means it occasionally re-reads what
// it already had.
esp_err_t wan_cfg_save(const wan_cfg_t *cfg);

// Incremented by every successful save.
uint32_t wan_cfg_generation(void);

// Validates a candidate configuration. Returns true and leaves *err_msg
// untouched on success; returns false and sets *err_msg to a static reason on
// failure. A disabled configuration always validates, so a half-filled form
// still saves.
bool wan_cfg_validate(const wan_cfg_t *cfg, const char **err_msg);

// Parses a comma-separated port list into cfg-shaped storage. An entry is a
// port number, optionally suffixed "/tcp" or "/udp"; a bare number is TCP,
// because TCP is what almost everything here wants and making the common case
// verbose would only invite mistakes in the list that matters most:
//
//     2101,2102,21115,21116,21116/udp,21117
//
// The same port may appear once per protocol - RustDesk genuinely needs both on
// 21116 - so the duplicate check is on the pair, not the number.
//
// Both the JSON API and the console hand their text to this, so the accepted
// syntax and the error messages live in one place - the same reasoning that
// keeps every other setting in this repo behind a shared validator. Whitespace
// around each entry is tolerated, an empty string yields zero rules (which
// wan_cfg_validate() then rejects for an enabled uplink), and a trailing comma
// is an error rather than a silently ignored empty field.
bool wan_cfg_parse_ports(const char *csv, wan_port_rule_t *out, uint8_t *out_n,
                         const char **err_msg);

// Formats a configuration's port list back to the syntax above. Returns the
// number of characters written, excluding the NUL. A buffer of
// WAN_CFG_PORTS_STR_MAX bytes is always enough. UDP rules carry their suffix;
// TCP ones do not, so what comes back out is what a person would have typed.
int wan_cfg_format_ports(const wan_cfg_t *cfg, char *out, size_t outsz);

// Fills in the compiled-in default rule set: NTRIP plus RustDesk. Exposed so
// the console and the docs cannot drift from what load_from_nvs() seeds.
void wan_cfg_default_ports(wan_port_rule_t *out, uint8_t *out_n);

#ifdef __cplusplus
}
#endif
