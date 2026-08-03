#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// Ring of the most recent log lines, so the web UI can show what until now
// needed a serial cable to see. Sized for ~23KB of static RAM: enough to hold
// the whole boot sequence, or a burst (WiFi association storm, OTA) without
// losing lines between the log page's 1Hz polls.
//
// TAG_MAX has to clear the longest tag IDF actually uses, not the longest one
// that looks plausible: "esp_eth.netif.netif_glue" is exactly 24 characters, so
// a 24-byte field (23 usable) dropped it and left the tag glued to the front of
// the message.
#define LOG_BUF_LINES     128
#define LOG_BUF_TAG_MAX   32
#define LOG_BUF_MSG_MAX   144

// Two independent thresholds, not one:
//
//   ESP_LOGx -> capture hook -+-> this ring   (gated by the global esp_log
//                             |                level, INFO by default)
//                             +-> UART        (only at or below the serial
//                                              level, WARN by default)
//
// The global level used to be pinned at WARN so routine INFO chatter wouldn't
// interleave with the console REPL sharing UART0 (see serial_console_init()).
// That would leave the log page empty almost all the time, so the global level
// now sits at INFO and the *serial* side does its own filtering here instead.
// Net effect on the serial console: unchanged.
//
// Note the ring can never be less verbose than the serial output, since only
// lines that passed the global filter reach the hook at all. "loglevel" keeps
// that invariant by *raising* the global level to meet what it was asked for,
// rather than by refusing - otherwise setting capture to "none" from the web
// page would silence the serial console with no way to undo it from the cable,
// which is backwards for a feature meant for diagnosing an unreachable device.

// Installs the esp_log vprintf hook. Call as early as possible in app_main() -
// anything logged before this is only ever seen on the serial console.
void log_buf_init(void);

// Serial-side threshold. `loglevel` on the console drives these, and drives the
// global level too - upward only, and only when the serial level it was asked
// for is above it.
void log_buf_set_serial_level(esp_log_level_t level);
esp_log_level_t log_buf_get_serial_level(void);

// Sequence numbers currently held (oldest..newest, inclusive). Numbering
// starts at 1 and restarts on reboot, so a reader whose cursor is suddenly
// ahead of `newest` knows the device restarted. `newest` is 0 while nothing
// has been logged yet. A reader that asked for a sequence older than `oldest`
// missed (oldest - cursor - 1) lines to overwrite.
//
// `missed` counts lines the capture hook could not store at all (lock
// contention) - the same drop-and-count-and-expose discipline the traffic
// accounting queue uses. It should stay at 0.
void log_buf_get_range(uint32_t *oldest, uint32_t *newest, uint32_t *missed);

// Copies out the line with the given sequence number. Returns false if it has
// already been overwritten, or hasn't happened yet.
//
// Callers must not hold any lock across this, and must not log from inside a
// sequence of these calls in a way that could recurse - see the note on
// s_mutex in log_buf.c.
bool log_buf_get_line(uint32_t seq, char *level, uint32_t *ts_ms,
                      char *tag, size_t tagsz, char *msg, size_t msgsz);

#ifdef __cplusplus
}
#endif
