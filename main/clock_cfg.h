#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Which timezone this device renders a wall-clock time in. The clock itself is
// clock_time.c; this module only stores and validates, so the console and the
// web handler can check a candidate setting without caring whether the clock
// has ever synchronised - the same split, and the same reason, as syslog_cfg.c
// and wan_cfg.c.
//
// Why a timezone is a stored setting rather than something worked out on the
// fly: the device has no way to know where it is. There is no GPS fix reaching
// this firmware, and geolocating the WAN's public address would be both a
// network call to somebody else's service and wrong for exactly the customer
// who matters - a hotspot roaming across a state line. So it is asked for once
// and remembered.
//
// Why the device formats at all, rather than sending an epoch and letting each
// reader render it: the same instant has to read identically on the web page,
// in the serial console and in the JSON, and a browser cannot parse a POSIX TZ
// string. Formatting here is what stops those three drifting - the arrangement
// reset_log_reason_name() already has for reset reasons.

// A POSIX TZ string, e.g. "CST6CDT,M3.2.0,M11.1.0". 40 covers every entry in
// the zoneinfo set with room over; the longest in common use is about 36.
#define CLOCK_CFG_TZ_MAX_LEN 40   // includes NUL

// UTC. The honest default: a device that has not been told where it is renders
// the one timezone that needs no local knowledge, and marks it, rather than
// guessing a zone and being quietly wrong by hours.
#define CLOCK_CFG_TZ_DEFAULT "UTC0"

typedef struct {
    char tz[CLOCK_CFG_TZ_MAX_LEN];
} clock_cfg_t;

// Must be called once (scheduler running, before any web or console access) to
// set up the RAM cache, for the same reason auth_cfg_init() and
// syslog_cfg_init() must be. Also applies the stored zone to the C library, so
// the first localtime_r() of the boot is already in the right zone.
esp_err_t clock_cfg_init(void);

// Current setting, from a RAM cache populated from NVS on first use. An empty
// stored string resolves to CLOCK_CFG_TZ_DEFAULT here, so no caller has to know
// that "unset" and "UTC" are the same thing.
void clock_cfg_get(clock_cfg_t *out);

// Persists a setting, updates the cache, and applies it to the C library with
// setenv("TZ")/tzset(). Unlike an SSID change this needs no reboot: localtime_r()
// picks the new zone up on its next call, and every timestamp this device
// renders goes through that.
esp_err_t clock_cfg_save(const clock_cfg_t *cfg);

// Validates a candidate setting. Returns true and leaves *err_msg untouched on
// success; returns false and sets *err_msg to a static reason on failure.
//
// The check is deliberately shallow - length, and the printable-ASCII rule the
// C library needs - because there is no way to tell a wrong POSIX TZ string
// from a right one. newlib parses what it recognises and silently treats the
// rest as UTC, so a typo cannot be rejected here; it shows up as a clock that
// reads wrong, which is why the UI shows the current time beside the field.
bool clock_cfg_validate(const clock_cfg_t *cfg, const char **err_msg);

// The compiled-in list of common zones the web form and the console offer, so
// the two cannot drift into presenting different choices. Entry i is a label
// ("America/Chicago") and its POSIX string; the caller stops when
// clock_cfg_zone(i, ...) returns false.
//
// A list rather than the full IANA database: tzdata is about 100KB of flash to
// carry every zone on earth for a device that will only ever be in one of them,
// and the field it saves the operator is one they can still type by hand.
bool clock_cfg_zone(int i, const char **label, const char **tz);

// Resolves an IANA label from that list ("America/Chicago") to the POSIX string
// the C library needs, case-insensitively. Returns NULL when the name is not in
// the list, which the caller should treat as "already a POSIX string" rather
// than as an error - anyone outside the built-in zones types their own.
//
// Lives here rather than in each caller for the reason
// syslog_cfg_severity_from_name() does: the console and the web handler must
// accept the same words, and a lookup copied into one of them is a lookup the
// other silently lacks. Getting that wrong is invisible - newlib parses what it
// recognises and treats the rest as UTC, so an unresolved label stores fine and
// renders hours out.
const char *clock_cfg_zone_from_label(const char *label);

#ifdef __cplusplus
}
#endif
