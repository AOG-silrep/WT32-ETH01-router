#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// What time it is, when the device is entitled to an opinion about that.
//
// reset_log.h used to open by arguing that nothing on this device could ever be
// timestamped, and one of its four reasons was that "a transparent L2 bridge has
// no uplink it can count on for SNTP". That stopped being true when the WAN
// landed. This module is the consequence: while the WAN is up it asks an NTP
// server what time it is, and everything else here exists to be careful about
// what that entitles the device to claim afterwards.
//
// The care is warranted because the clock's failure mode is silent. An uptime
// counter that stops is obviously broken; a wall clock that is wrong reads
// exactly like one that is right, and a reset history dated with a wrong clock
// is worse than one with no dates at all - it invites a diagnosis rather than
// merely failing to support one. So every surface asks first, and renders
// nothing rather than something plausible.
//
// WHAT SURVIVES WHAT. The wall clock has exactly the survival profile the RTC
// uptime counter in reset_log.c has, which is a coincidence worth leaning on
// rather than working around. With CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER and
// CONFIG_ESP_TIME_FUNCS_USE_ESP_TIMER both set (they are, see sdkconfig), IDF
// keeps the clock's origin in RTC registers and restores it from there on every
// startup - esp_libc_time_init() calls esp_set_time_from_rtc(). So the clock
// survives a panic, a watchdog, a software reset and an OTA restart, and is
// wiped by power-on.
//
// That is what lets a boot following a crash stamp its own reset record
// immediately, with no WAN and no sync, which is the case a field diagnosis
// most often turns on. Only a boot following a power-on has to wait.
//
// WHY A FLAG AND NOT A HEURISTIC. IDF exposes no "is the time valid" bit, and
// the usual workaround - test whether time(NULL) is past some sentinel year -
// cannot tell a synchronised clock from RTC garbage that happened to land past
// the sentinel, which on a 32-bit second count is most of it. So this module
// keeps its own flag in RTC memory, written only after a real sync, with the
// same complement-pair validation reset_log.c uses and for the same reason.
//
// HOW GOOD IS IT. CONFIG_RTC_CLK_SRC_INT_RC is set: the RTC runs off the
// internal RC oscillator rather than a crystal, and drifts accordingly. While
// the WAN is up SNTP re-syncs hourly and the drift never accumulates. While it
// is down nothing corrects it, so a device that syncs and then spends a week
// offline is telling you what its RC oscillator thinks, and clock_time_stale()
// is how a surface finds that out before printing seconds as though they meant
// something.

// Starts the SNTP client machinery. Does not itself go to the network: nothing
// is sent until the WAN comes up and clock_time_wan_up() is called.
//
// Must run after clock_cfg_init() (which applies the timezone) and after
// nvs_flash_init(). Returns non-OK only on an allocation failure. Like
// reset_log_init(), call it WITHOUT ESP_ERROR_CHECK - a device that cannot tell
// the time must still bridge.
esp_err_t clock_time_init(void);

// Driven from wan.c's event handlers rather than by polling wan_is_up(): the
// client starts when the WAN has an address and stops when it loses one.
void clock_time_wan_up(void);
void clock_time_wan_down(void);

// The current time, or false when the device has no business claiming one.
// False means exactly one thing: no successful sync since the last power-on,
// and no clock inherited through RTC from a boot that did sync.
bool clock_time_now(time_t *out);

// True when the clock is believable but has not been corrected in long enough
// that the RC oscillator's drift is worth disclosing. Always false while the
// WAN is up and syncing.
bool clock_time_stale(void);

// When the last successful sync happened, as a wall-clock time, or 0 if there
// has never been one this power-cycle. Note this can be non-zero while
// clock_time_synced() is true via an inherited RTC clock and no sync has
// happened in THIS boot - the two answer different questions.
time_t clock_time_last_sync(void);

// Where the current time came from, as a stable token for the JSON, the console
// and the web page - the arrangement reset_log_reason_name() has, and for the
// same reason: three surfaces describing one device must not invent three
// vocabularies. Never NULL.
//
//   "none"    - no clock
//   "ntp"     - synchronised from an NTP server this boot
//   "carried" - inherited through RTC memory from an earlier boot that synced
const char *clock_time_source(void);

// Renders a time in the configured zone, as "2026-08-22 21:15:12". Buffer
// should be CLOCK_TIME_STR_MAX bytes. The single formatter every surface goes
// through, so that a record cannot read one way in the console and another on
// the web page.
//
// Local rather than UTC, and per-record rather than by applying one current
// offset to everything: localtime_r() applies the rule in force at THAT
// instant, so a record from January renders correctly when read in July.
#define CLOCK_TIME_STR_MAX 20   // "YYYY-MM-DD HH:MM:SS" + NUL
void clock_time_format(time_t t, char *out, size_t outsz);

// The zone's short name for the instant given ("CST", "CDT", "UTC"), for
// surfaces that show one time and need to say which zone it is in. Never NULL.
const char *clock_time_zone_abbrev(time_t t);

// Call about once a second from an ordinary task. Performs the one deferred
// piece of work this module has: writing this boot's start time back into its
// reset record after the first sync of the boot.
//
// Deferred to a task, and to this function, for the reason reset_log.h gives
// about reset_log_checkpoint_tick() - the write can block for the length of a
// flash write, and the SNTP callback that triggers it runs in the tcpip task.
// It shares a caller with that function in sys_monitor.c. Harmless before
// clock_time_init(), or after one that failed; it does nothing.
void clock_time_tick(void);

#ifdef __cplusplus
}
#endif
