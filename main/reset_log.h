#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Why this device restarted, kept in flash so the answer outlives the restart.
//
// The case this exists for: a bridge that was found rebooted. The log ring the
// web log page reads lives in RAM (see log_buf.c), so it begins at this boot's
// log_buf_init() and whatever explained the last reboot is gone. esp_reset_reason()
// does answer "brownout or panic?", but only for the boot you are standing in
// front of, and only if somebody had a serial cable attached at that second -
// which is never true of the boot that actually mattered. A device installed in
// a field erases its own evidence every time it comes back up.
//
// Nothing here is timestamped, and nothing here can be. This device cannot
// measure elapsed time across a reboot: esp_timer_get_time() restarts at 0 every
// boot, RTC memory is wiped by power-on, there is no RTC battery, and a
// transparent L2 bridge has no uplink it can count on for SNTP. dhcp_server.c
// states the same constraint for lease generations, and the consequence is the
// same here - a board unplugged for a month and one power-cycled a second ago
// produce identical records. What is recoverable is the *order* of resets and
// the *duration* of the boot each one ended, and those two turn out to be what
// the diagnosis actually needs: "panicked three times, each about ninety
// seconds in" is a finding. "Panicked at 4:12am" would be nicer and is not on
// offer.
//
// Duration comes from a counter in RTC slow memory, refreshed once a second at
// runtime and costing nothing to keep. It survives software resets, panics,
// watchdogs, and the brownout-interrupt path. Power-on wipes it, and that wipe
// is not a gap in the data, it *is* the discriminator: a record that comes back
// with no previous state is a record of a power event.
//
// Which left the power case - the one a field installation cares most about -
// as the only case with no duration at all, and a run of undated power-ons
// cannot tell a boot loop from a flaky supply from somebody switching the thing
// off at night. So the counter is also checkpointed to flash, on an interval
// that widens with the uptime it is recording. The schedule is a fixed list of
// thirty-two times running from ten seconds out to a year, dense at the start
// where the diagnosis lives and roughly doubling towards the end; past a year it
// stops. That makes the cost a cap rather than a rate - at most thirty-two writes
// plus the seed for the entire life of a boot - which the flash absorbs without
// noticing (see reset_log.c for the list and the arithmetic).
//
// Every boot also seeds a checkpoint of zero as it starts, sharing the commit
// that writes this ring - so a checkpoint is present for every boot that got as
// far as running this code, and its ABSENCE means something specific rather than
// something vague: no data at all, because the boot ran firmware from before
// this existed or because its one NVS write failed. Without that seed, "no
// checkpoint" would also cover "the boot ended before the first one was due",
// and the two are not the same claim - reading the second as the first would let
// a device serial-flashed from an older build report a boot that ran for a week
// as having died in ten seconds.
//
// What comes back is therefore a BOUND, never an exact figure: the boot lasted
// at least as long as the last checkpoint it managed to write. The surfaces mark
// it (">30s", ">5m", ">1y"), and use "<" for the one case with no lower bound
// worth stating - a boot that only ever wrote its seed, which reads "<10s".
// reset_log_uptime_ceiling() gives the other end for anything that wants the
// whole window rather than the mark. The RTC counter still wins wherever it
// survived: it is exact, renders without a mark, and a reset that preserved it
// has nothing to gain from the flash copy. What the checkpoint does not do is
// reclassify anything. A power-on record with a duration on it is still
// "power-on or power loss", for the reason below.
//
// The limit of that discriminator, which every surface has to respect: a sag
// that trips the brownout detector while the chip is still running reports
// honestly as a brownout, with its uptime intact. A rail that collapses past the
// chip's own power-on-reset threshold does not - it is indistinguishable from
// somebody flipping a switch, and no amount of firmware changes that. So a
// power-on record is never labelled "power loss" anywhere; it is labelled
// "power-on or power loss", and the actual field diagnosis is a *pattern* - a
// run of them with short durations on a device nobody is switching off is a
// supply problem. (CONFIG_ESP_BROWNOUT_DET_LVL is 0, the most permissive trip
// point, so the honest-brownout path fires late. Raising it would convert more
// sags into real brownout records; that is an sdkconfig change and deliberately
// not part of this.)
//
// boot_seq counts boots that reached app_main, not power-ups. A bootloader-level
// failure - a rejected signature, a corrupt image - never gets a record and
// leaves no visible gap, so every surface says "boots recorded" rather than
// "boots".
//
// This history is deliberately kept across factory-reset: it is evidence about
// the device rather than configuration of it, and a factory reset is often the
// first thing tried on a device that keeps restarting. The one thing that does
// erase it is the nvs_flash_erase() recovery path in app_main, which fires only
// when NVS itself is unusable.

#define RESET_LOG_MAX_RECORDS 16

// esp_app_desc_t.version holds 32; 24 clears a "1.3.0-12-gabc1234" git-describe
// string, which is what a later build system would most likely produce.
#define RESET_LOG_VERSION_MAX 24

// "ota_0" / "ota_1" / "factory" all fit.
#define RESET_LOG_PART_MAX 8

// Why a restart happened, when software asked for it. UNKNOWN is the
// interesting value rather than the boring one: nothing on this device restarts
// itself without tagging it first, so an untagged software reset is a panic that
// got tidied up on the way out, or a code path nobody accounted for.
typedef enum {
    RESET_INTENT_UNKNOWN = 0,
    RESET_INTENT_OTA,
    RESET_INTENT_WIFI_SAVE,
    RESET_INTENT_CONSOLE,
    RESET_INTENT_FACTORY_RESET,
} reset_intent_t;

#define RESET_LOG_F_PREV_STATE    0x01  // prev_uptime_s/intent/reached-ready are real
#define RESET_LOG_F_REACHED_READY 0x02  // the previous boot finished app_main
#define RESET_LOG_F_OTA_PENDING   0x04  // this boot is a trial image, not yet marked valid
#define RESET_LOG_F_ROLLBACK      0x08  // the bootloader reverted a trial image to get here

// prev_uptime_s came off the flash checkpoint rather than the RTC counter, so it
// is the lower end of a window and not a reading: the previous boot lasted at
// least that long, and less than reset_log_uptime_ceiling() of it. Mutually
// exclusive with F_PREV_STATE - when the RTC block survived there is an exact
// figure and this is not set - so "prev_uptime_s means something" is
// (F_PREV_STATE | F_PREV_UPTIME_MIN).
//
// Zero is a real value here, not an absent one: it is the seed every boot writes
// as it starts, and it says the boot ended before the first scheduled checkpoint
// - "under ten seconds", which is a finding. A record with neither flag is the
// one that knows nothing.
//
// It carries the duration and nothing else. intent is unknown and reached-ready
// is unknowable on this path, which is why neither may be read off a record
// holding only this flag - the checkpoint is written long after startup finished
// and says nothing about how startup went.
#define RESET_LOG_F_PREV_UPTIME_MIN 0x10

// What the 3.3V rail did across the reset, from a witness held outside the chip
// - see rail_witness.h for how it is read and what it costs to believe.
//
// A gate bit and a value bit, the same shape as F_PREV_STATE and prev_uptime_s,
// because absent and false are different answers: F_RAIL_KNOWN clear means
// nothing was measured, and only under it does F_RAIL_HELD mean the rail stayed
// up. Recorded on every record regardless of reason, though it only tells the
// reader anything on a power-on one - every other reset reason preserved RTC
// memory, which already proves the rail held. Kept on the rest as a check on the
// witness itself: a panic record claiming the rail dropped means the witness is
// lying, not that the device lost power and panicked about it.
//
// F_RAIL_HELD is not "somebody reset the board". It says the rail stayed above
// the PHY's retention threshold, which an EN-pin reset does and a power cut does
// not - but so would a sag deep enough to reset the ESP32 and no deeper. The
// surfaces have to say what was measured and let the reader draw the rest.
#define RESET_LOG_F_RAIL_KNOWN 0x20
#define RESET_LOG_F_RAIL_HELD  0x40

// One reset event, written at the start of the boot that followed it. The two
// halves describe different boots on purpose: reason, intent and prev_uptime_s
// are how the PREVIOUS boot ended, version and part are what came up afterwards.
// Reading it as one sentence is the point - "panicked after 4h12m, came back on
// 1.3.0 from ota_0".
typedef struct {
    uint32_t boot_seq;                       // 1-based; the boot this record opened
    uint32_t prev_uptime_s;                  // exact under F_PREV_STATE, a floor
                                             // under F_PREV_UPTIME_MIN, and
                                             // meaningless under neither
    uint8_t  reason;                         // esp_reset_reason_t, stored raw
    uint8_t  intent;                         // reset_intent_t
    uint8_t  flags;                          // RESET_LOG_F_*
    char     version[RESET_LOG_VERSION_MAX]; // always NUL-terminated
    char     part[RESET_LOG_PART_MAX];       // always NUL-terminated
} reset_log_entry_t;

// Reads why this boot happened, appends a record for it to the ring in flash,
// and arms the uptime counter for this boot.
//
// Must run after nvs_flash_init() and before anything that can restart or crash.
// A boot that dies in eth_init() must not be the one that loses the record of
// the last five times it did exactly that, which is the case this module exists
// for. It needs nothing else: esp_reset_reason() was latched by the startup code
// long before app_main, and the OTA state calls read otadata directly.
//
// Returns non-OK only when it could not allocate its mutex or its timer. A
// failed NVS read or write is logged and swallowed - this module is evidence,
// not infrastructure, and must never be the reason a bridge stops bridging.
// Call it WITHOUT ESP_ERROR_CHECK, unlike its neighbours in app_main.
//
// rail_held is what the rail witness made of this reset, and is passed in rather
// than read here so this module keeps knowing nothing about Ethernet. Pass
// RESET_RAIL_UNKNOWN and the record simply carries no rail flags, which is what
// a build without a witness, or a board whose PHY would not hold one, produces.
typedef enum {
    RESET_RAIL_UNKNOWN = 0,
    RESET_RAIL_HELD,
    RESET_RAIL_DROPPED,
} reset_rail_t;

esp_err_t reset_log_init(reset_rail_t rail);

// Tags the restart that is about to be requested, so the next boot can say a
// person asked for it. Call immediately before esp_restart(), ahead of any
// settling delay. Also stops the tick timer and takes a final uptime sample, so
// a deliberate reboot reports its length exactly rather than to the nearest
// second. Safe to call before reset_log_init(); it does nothing.
void reset_log_note_intent(reset_intent_t intent);

// Records that startup got all the way through. Costs no flash - it sets one
// word of RTC memory. What it buys: an uptime of three seconds is either a boot
// loop or a device that was switched off three seconds after it came up, and
// nothing else here can tell those apart.
void reset_log_note_ready(void);

// The open upper end of the window a F_PREV_UPTIME_MIN figure sits in: the next
// checkpoint the previous boot would have written, had it survived to write it.
// So a floor of 0 returns 10, one of 300 returns 600, and the reading is "at
// least floor_s, less than this".
//
// Returns 0 when the schedule holds nothing further, which is the case for a boot
// that had been up more than a year - there the reading is "at least floor_s"
// with no upper end at all, and callers must not print a 0 as if it were one.
//
// Derived from the running build's schedule rather than stored in the record. A
// later build that changes the list would therefore recompute the ceilings of
// records already in the ring. That needs a serial reflash to happen at all - an
// OTA restart preserves RTC memory, so the boot after one never consults a
// checkpoint - and is not worth a stored-format version to avoid.
uint32_t reset_log_uptime_ceiling(uint32_t floor_s);

// Writes this boot's uptime to flash, if it has reached the next point on the
// schedule. Call about once a second; a late call costs accuracy in the recovered
// figure and nothing else - one delayed past several points still writes once -
// and calling it more often than once a second does not make it write more often.
//
// Must be called from an ordinary task, never from a timer callback: it can
// block for the length of a flash write. That is also why it does not live on
// this module's own 1 Hz timer - stalling that timer would freeze the uptime
// counter the whole module is built on, to record a duration less accurately.
// Harmless before reset_log_init(), or after one that failed; it does nothing.
void reset_log_checkpoint_tick(void);

// Copies up to max records, NEWEST FIRST, and returns the number written.
// Thread-safe; intended for the HTTP server and console tasks.
int reset_log_get(reset_log_entry_t *out, int max);

// The record for the boot currently running - what /api/system summarises.
// False only if reset_log_init() has not run, or failed. Thread-safe.
bool reset_log_get_current(reset_log_entry_t *out);

// Stable tokens shared by the JSON, the console and the web page, so the three
// cannot drift into describing the same device differently - the same
// arrangement log_level_name() has in web_server.c. Never NULL: a reason code
// this build does not know returns "unknown", which is why /api/resets also
// carries the raw code.
const char *reset_log_reason_name(uint8_t reason);
const char *reset_log_intent_name(uint8_t intent);

#ifdef __cplusplus
}
#endif
