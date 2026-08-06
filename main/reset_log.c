#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "reset_log.h"
#include "esp_attr.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "reset_log";

// ---- NVS ----

#define NVS_NAMESPACE "reset_log"
#define NVS_RING_KEY  "ring"
#define NVS_CKPT_KEY  "upck"
#define STORE_MAGIC   0x474C5352  // 'RSLG'
#define STORE_VERSION 1

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t count;     // records that follow, <= RESET_LOG_MAX_RECORDS
    uint32_t next_seq;  // boot_seq to hand the next record
} stored_hdr_t;

// Constant at version 1, unlike dhcp_server.c's, which already has a version to
// be older than. Kept in the same shape anyway, because it marks the seam: the
// load path reads the version out of a prefix before trusting the rest of the
// header, so a version 2 that grows this struct has somewhere to grow into
// without a flag day.
#define STORED_HDR_LEN(ver) (sizeof(stored_hdr_t))

typedef struct __attribute__((packed)) {
    uint32_t boot_seq;
    uint32_t uptime_s;
    uint8_t  reason;
    uint8_t  intent;
    uint8_t  flags;
    uint8_t  rsvd;    // 0; keeps the struct 4-aligned and gives version 2 a byte
    char     version[RESET_LOG_VERSION_MAX];
    char     part[RESET_LOG_PART_MAX];
} stored_rec_t;

// 12 + 16 * 44 = 716 bytes, comfortably inside NVS's 1984-byte single-page blob
// chunk, so the blob never splits across pages of the 24KB partition (see
// partitions.csv) that wifi_config, auth_cfg and dhcp_leases already share.
//
// This blob is written once per boot and never at runtime, which is what keeps
// the uptime checkpoint below cheap: rewriting 716 bytes on every checkpoint
// would cost 23 NVS entries a time instead of one. Sixteen records is chosen
// against the reader, not the flash: someone diagnosing a device that keeps
// restarting needs enough rows to see whether the pattern repeats, and sixteen
// shows that while still fitting one screen and one response buffer.
#define STORED_BLOB_MAX (sizeof(stored_hdr_t) + RESET_LOG_MAX_RECORDS * sizeof(stored_rec_t))

// ---- uptime checkpoint ----

// The RTC counter answers "how long did the last boot run" for every reset that
// left RTC memory standing. Power-on is the exception, and the one that matters
// most in the field, so the counter is also copied to flash periodically and
// read back on the next boot when RTC has nothing to offer.
//
// Stored as a single u64, boot_seq in the high half and seconds in the low half,
// through nvs_set_u64 rather than a blob: primitive types live inside one 32-byte
// NVS entry, so a checkpoint costs exactly one entry and NVS's own per-entry CRC
// covers it - no complement pairs needed here, unlike the RTC block, which has no
// integrity check of its own to lean on.
//
// The boot_seq half is what makes the value self-invalidating. A checkpoint is
// only believed by the boot whose sequence number is exactly one past it, so a
// value left behind by an older boot is discarded rather than misattributed -
// reporting a stale duration would be worse than reporting none.
//
// Every boot seeds a checkpoint of zero from reset_log_init(), riding the commit
// that writes the ring blob, which costs one 32-byte entry and no extra commit.
// That seed is what gives the absence of a checkpoint a single meaning: no data,
// because the previous boot predates this code or its one NVS write failed. A
// present checkpoint of zero means something quite different and much more
// useful - the boot got as far as starting and then ended before the first
// scheduled write, i.e. inside ten seconds, which on a device nobody is
// switching off is a supply collapsing under load rather than a clean power-off.
//
// The seed is a literal zero rather than the uptime at init, so the window
// [0, 10s) it implies is right however long init itself took. Seeding the real
// reading would put the floor at 1 on any boot whose init crossed a second
// boundary, which is a worse answer for no gain.
//
// The schedule is a list of times, not a rule for generating them. Earlier
// versions expressed it as tiers - "up to X, write every Y" - and every retune
// of it had to be re-derived, landed its boundaries wherever the arithmetic put
// them, and hid its proportionality in the interaction between rows. Written out,
// the property that matters is just visible: no step is more than double the one
// before it, so the uncertainty in a recovered figure stays proportional to the
// figure all the way out. Dense at the start because that is where the diagnosis
// lives - telling a four-second boot from a forty-second one is the whole
// question on a device that keeps dying - and geometric after that because an
// eight-day uptime does not need ten-second resolution.
#define CKPT_M (60u)
#define CKPT_H (60u * CKPT_M)
#define CKPT_D (24u * CKPT_H)
#define CKPT_Y (365u * CKPT_D)

static const uint32_t k_ckpt_times[] = {
             10,          20,          30,          40,          50,
       1 * CKPT_M,  2 * CKPT_M,  3 * CKPT_M,  4 * CKPT_M,  5 * CKPT_M,
      10 * CKPT_M, 15 * CKPT_M, 30 * CKPT_M, 45 * CKPT_M,
       1 * CKPT_H,  2 * CKPT_H,  3 * CKPT_H,  4 * CKPT_H,
       8 * CKPT_H, 12 * CKPT_H, 18 * CKPT_H,
       1 * CKPT_D,  2 * CKPT_D,  3 * CKPT_D,  4 * CKPT_D,
       8 * CKPT_D, 16 * CKPT_D, 32 * CKPT_D, 64 * CKPT_D,
     128 * CKPT_D, 256 * CKPT_D,
       1 * CKPT_Y,
};

#define CKPT_COUNT (sizeof(k_ckpt_times) / sizeof(k_ckpt_times[0]))

// Past the last entry the schedule simply ends and this module stops writing.
// The stored floor stays at a year and the record reads ">1y", which does not
// stop being true however much longer the device runs - so there is nothing to
// gain by continuing to spend flash on it.
//
// That makes the whole cost a hard cap rather than a rate: at most 32 writes plus
// the seed for the entire life of a boot, where every earlier version of this was
// so many per year. A boot lasting a day reaches 22 of them, against the 23 NVS
// entries the 716-byte ring blob spends on that same boot - so the ring is still
// the dominant per-boot cost, and a device that boot-loops is paced by that
// rather than by this. An NVS page holds 126 entries and the part is rated for
// 100,000 erase cycles; nothing here is close to either.
static uint32_t s_ckpt_boot_seq;
static size_t s_ckpt_next;   // index of the next scheduled write

// The next scheduled write after floor_s, or 0 when the schedule has no more -
// see the header for why this is derived rather than stored. A plain scan: the
// table is 32 entries and this runs when a page is rendered, not on any hot path.
static uint32_t ceiling_for(uint32_t floor_s)
{
    for (size_t i = 0; i < CKPT_COUNT; i++) {
        if (k_ckpt_times[i] > floor_s) {
            return k_ckpt_times[i];
        }
    }
    return 0;
}

// ---- RTC live state ----

#define RESET_LOG_RTC_MAGIC 0x52534C31  // 'RSL1'

// Complement pairs rather than one checksum over the block: each fact validates
// on its own, so a reset landing mid-update costs that field only and there is
// no write-ordering subtlety to get wrong. The magic is a separate gate for "did
// this module ever write here at all" - RTC slow memory comes up uninitialised,
// not zeroed, and a one-word magic matching out of power-on garbage is a coin
// this module should not be flipping at three in the morning.
typedef struct {
    uint32_t magic;
    uint32_t uptime_s,      uptime_s_inv;
    uint32_t intent,        intent_inv;
    uint32_t reached_ready, reached_ready_inv;
} reset_log_rtc_t;

static RTC_NOINIT_ATTR reset_log_rtc_t s_rtc;   // 28 bytes of 8KB RTC slow memory

// Not mutex-guarded, and that is deliberate rather than an oversight. Every
// field is a 32-bit aligned store, atomic on this core, and the writers touch
// disjoint fields: the tick callback writes uptime_s, reset_log_note_ready()
// writes reached_ready. The one overlap is reset_log_note_intent(), which also
// refreshes uptime - and it stops the timer before doing so, which it wants to
// do anyway since the device is about to restart.

static esp_timer_handle_t s_tick_timer;

// ---- guarded ring ----

// Strictly this is written once, during reset_log_init(), before the HTTP
// server or console tasks exist to read it - so the lock buys nothing today.
// It is taken anyway: the cost is nil off the packet path, and "only ever
// written at boot" is an invariant that a later feature (a DELETE /api/resets,
// a record appended on graceful shutdown) would break silently rather than
// loudly. Same shape as sys_monitor.c and eth_link.c, not the bare-volatile
// counter style client_track.c uses to buy its way out of locking on the hot
// path.
static SemaphoreHandle_t s_mutex;
static reset_log_entry_t s_ring[RESET_LOG_MAX_RECORDS];  // oldest first
static int s_count;
static uint32_t s_next_seq = 1;
static bool s_have_current;

const char *reset_log_reason_name(uint8_t reason)
{
    // A switch rather than an array indexed by the enum: esp_reset_reason_t
    // values are not a contract across IDF versions, and a record read back out
    // of flash may have been written by a different build than this one.
    switch ((esp_reset_reason_t)reason) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int-wdt";
    case ESP_RST_TASK_WDT:  return "task-wdt";
    case ESP_RST_WDT:       return "other-wdt";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
    }
}

const char *reset_log_intent_name(uint8_t intent)
{
    switch ((reset_intent_t)intent) {
    case RESET_INTENT_OTA:           return "ota";
    case RESET_INTENT_WIFI_SAVE:     return "wifi-save";
    case RESET_INTENT_CONSOLE:       return "console";
    case RESET_INTENT_FACTORY_RESET: return "factory-reset";
    case RESET_INTENT_UNKNOWN:       return "unknown";
    default:                         return "unknown";
    }
}

// ---- NVS load/save ----

// Just the set, so the seed can ride the ring blob's commit rather than paying
// for one of its own. The caller owns the handle and the commit.
static esp_err_t set_checkpoint(nvs_handle_t h, uint32_t uptime_s)
{
    uint64_t packed = ((uint64_t)s_ckpt_boot_seq << 32) | uptime_s;
    return nvs_set_u64(h, NVS_CKPT_KEY, packed);
}

static void load_from_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no reset history in flash (%s) - starting one", esp_err_to_name(err));
        return;
    }

    uint8_t buf[STORED_BLOB_MAX];
    size_t len = sizeof(buf);
    err = nvs_get_blob(h, NVS_RING_KEY, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no reset history in flash (%s) - starting one", esp_err_to_name(err));
        return;
    }

    if (len < STORED_HDR_LEN(STORE_VERSION)) {
        ESP_LOGW(TAG, "saved reset history truncated (%u bytes) - starting over",
                 (unsigned)len);
        return;
    }
    stored_hdr_t hdr = { 0 };
    memcpy(&hdr, buf, STORED_HDR_LEN(STORE_VERSION));
    if (hdr.magic != STORE_MAGIC || hdr.version < 1 || hdr.version > STORE_VERSION) {
        ESP_LOGW(TAG, "saved reset history is not a ring this build understands "
                      "(magic %08x, version %u) - starting over",
                 (unsigned)hdr.magic, (unsigned)hdr.version);
        return;
    }

    size_t hdr_len = STORED_HDR_LEN(hdr.version);
    if (hdr.count > RESET_LOG_MAX_RECORDS ||
        len < hdr_len + (size_t)hdr.count * sizeof(stored_rec_t)) {
        ESP_LOGW(TAG, "saved reset history claims %u records in %u bytes - starting over",
                 (unsigned)hdr.count, (unsigned)len);
        return;
    }

    for (int i = 0; i < hdr.count; i++) {
        stored_rec_t s;
        memcpy(&s, buf + hdr_len + (size_t)i * sizeof(stored_rec_t), sizeof(s));
        s_ring[i].boot_seq = s.boot_seq;
        s_ring[i].prev_uptime_s = s.uptime_s;
        s_ring[i].reason = s.reason;
        s_ring[i].intent = s.intent;
        s_ring[i].flags = s.flags;
        memcpy(s_ring[i].version, s.version, RESET_LOG_VERSION_MAX);
        memcpy(s_ring[i].part, s.part, RESET_LOG_PART_MAX);
        // The header promises these are NUL-terminated. Enforce that at the
        // flash boundary rather than assuming it: the bytes came off a medium,
        // possibly written by a different firmware image.
        s_ring[i].version[RESET_LOG_VERSION_MAX - 1] = '\0';
        s_ring[i].part[RESET_LOG_PART_MAX - 1] = '\0';
    }
    s_count = hdr.count;
    s_next_seq = hdr.next_seq ? hdr.next_seq : 1;

    ESP_LOGI(TAG, "restored %d reset record%s from flash", s_count, s_count == 1 ? "" : "s");
}

static void save_to_nvs(void)
{
    uint8_t buf[STORED_BLOB_MAX];
    stored_hdr_t hdr = {
        .magic = STORE_MAGIC,
        .version = STORE_VERSION,
        .count = (uint16_t)s_count,
        .next_seq = s_next_seq,
    };
    memcpy(buf, &hdr, sizeof(hdr));

    size_t off = sizeof(hdr);
    for (int i = 0; i < s_count; i++) {
        stored_rec_t s = { 0 };
        s.boot_seq = s_ring[i].boot_seq;
        s.uptime_s = s_ring[i].prev_uptime_s;
        s.reason = s_ring[i].reason;
        s.intent = s_ring[i].intent;
        s.flags = s_ring[i].flags;
        memcpy(s.version, s_ring[i].version, RESET_LOG_VERSION_MAX);
        memcpy(s.part, s_ring[i].part, RESET_LOG_PART_MAX);
        memcpy(buf + off, &s, sizeof(s));
        off += sizeof(s);
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_RING_KEY, buf, off);
        if (err == ESP_OK) {
            // This boot's uptime checkpoint, seeded at zero, riding the same
            // commit because both are once-per-boot writes to the same handle.
            // Tied to the ring on purpose: if the blob did not land, the next
            // boot has no record to hang a duration on either, so the two
            // failing together is the consistent outcome.
            err = set_checkpoint(h, 0);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }

    // Logged and dropped, not retried and not fatal. The in-RAM ring already
    // holds this boot's record, so both surfaces stay correct for this session -
    // only the part that outlives the reboot is lost, and refusing to boot over
    // that would be worse than losing it.
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save reset history: %s", esp_err_to_name(err));
    }
}

// Whether the boot numbered *want_seq* left a checkpoint, and if so how far it
// had got. False is "no data" and says nothing about how long that boot ran;
// true with *out_s == 0 is the seed, and says it ended before the first
// scheduled write. Returning 0 for both, as an earlier version did, would have
// collapsed exactly the distinction the seed exists to draw.
static bool load_checkpoint(uint32_t want_seq, uint32_t *out_s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return false;
    }

    uint64_t packed = 0;
    err = nvs_get_u64(h, NVS_CKPT_KEY, &packed);
    nvs_close(h);
    if (err != ESP_OK) {
        return false;
    }

    uint32_t seq = (uint32_t)(packed >> 32);
    if (seq != want_seq) {
        // Left by an older boot: the one that just ended never got as far as
        // seeding, so nothing here describes it.
        ESP_LOGD(TAG, "uptime checkpoint is for boot #%u, wanted #%u - discarding",
                 (unsigned)seq, (unsigned)want_seq);
        return false;
    }
    *out_s = (uint32_t)packed;
    return true;
}

static void save_checkpoint(uint32_t uptime_s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = set_checkpoint(h, uptime_s);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }

    if (err != ESP_OK) {
        // Same policy as the ring: logged and dropped. A checkpoint that failed
        // to land costs the next boot a duration it would have liked to have,
        // and costs this boot nothing at all.
        ESP_LOGW(TAG, "failed to checkpoint uptime: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGD(TAG, "uptime checkpoint: %us", (unsigned)uptime_s);
}

uint32_t reset_log_uptime_ceiling(uint32_t floor_s)
{
    return ceiling_for(floor_s);
}

// ---- init ----

static void tick_cb(void *arg)
{
    // Samples esp_timer, it does not increment a counter. A callback that runs
    // late or not at all therefore cannot accumulate error: the stored value is
    // always esp_timer's own uptime, at worst one period stale. Incrementing
    // would turn every scheduling hiccup into a permanent under-count, on a
    // device where the interesting boots are exactly the ones with scheduling
    // hiccups.
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    s_rtc.uptime_s = up;
    s_rtc.uptime_s_inv = ~up;
}

static void rtc_seed(void)
{
    s_rtc.uptime_s = 0;
    s_rtc.uptime_s_inv = ~0u;
    s_rtc.intent = RESET_INTENT_UNKNOWN;
    s_rtc.intent_inv = ~(uint32_t)RESET_INTENT_UNKNOWN;
    s_rtc.reached_ready = 0;
    s_rtc.reached_ready_inv = ~0u;
    s_rtc.magic = RESET_LOG_RTC_MAGIC;
}

static void describe_for_log(const reset_log_entry_t *e, char *out, size_t n)
{
    if (e->flags & RESET_LOG_F_PREV_STATE) {
        snprintf(out, n, "%s after %us%s%s%s",
                 reset_log_reason_name(e->reason),
                 (unsigned)e->prev_uptime_s,
                 (e->intent != RESET_INTENT_UNKNOWN) ? " (" : "",
                 (e->intent != RESET_INTENT_UNKNOWN) ? reset_log_intent_name(e->intent) : "",
                 (e->intent != RESET_INTENT_UNKNOWN) ? ")" : "");
    } else if (e->flags & RESET_LOG_F_PREV_UPTIME_MIN) {
        // No intent clause on this path - the checkpoint carries a duration and
        // nothing else, and an untagged reset here means "not recorded" rather
        // than "nobody asked for it". Raw seconds rather than the surfaces'
        // formatting: this is one line in a log a developer is already reading.
        if (e->prev_uptime_s == 0) {
            snprintf(out, n, "%s in under %us", reset_log_reason_name(e->reason),
                     (unsigned)ceiling_for(0));
        } else {
            snprintf(out, n, "%s after more than %us", reset_log_reason_name(e->reason),
                     (unsigned)e->prev_uptime_s);
        }
    } else {
        snprintf(out, n, "%s, previous uptime unknown", reset_log_reason_name(e->reason));
    }
}

esp_err_t reset_log_init(reset_rail_t rail)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // Read the RTC block before anything can disturb it. Valid means the magic
    // matches AND all three complements do; any failure and the previous boot's
    // state reports as unknown rather than as zero, because zero would be a
    // claim. This is the normal case after every power cycle, so it must not
    // read as a fault anywhere downstream.
    bool prev_valid = (s_rtc.magic == RESET_LOG_RTC_MAGIC) &&
                      (s_rtc.uptime_s == ~s_rtc.uptime_s_inv) &&
                      (s_rtc.intent == ~s_rtc.intent_inv) &&
                      (s_rtc.reached_ready == ~s_rtc.reached_ready_inv);
    uint32_t prev_uptime = prev_valid ? s_rtc.uptime_s : 0;
    uint32_t prev_intent = prev_valid ? s_rtc.intent : RESET_INTENT_UNKNOWN;
    bool prev_ready = prev_valid ? (s_rtc.reached_ready != 0) : false;
    if (!prev_valid && s_rtc.magic == RESET_LOG_RTC_MAGIC) {
        ESP_LOGD(TAG, "RTC state present but inconsistent - previous boot reports as unknown");
    }

    // Re-seeded immediately, before anything below can fail, so this boot's
    // counter is armed even if the NVS work goes wrong.
    rtc_seed();

    esp_reset_reason_t reason = esp_reset_reason();

    load_from_nvs();

    reset_log_entry_t rec = { 0 };
    rec.reason = (uint8_t)reason;
    if (prev_valid) {
        rec.flags |= RESET_LOG_F_PREV_STATE;
        rec.prev_uptime_s = prev_uptime;
        rec.intent = (uint8_t)prev_intent;
        if (prev_ready) {
            rec.flags |= RESET_LOG_F_REACHED_READY;
        }
    } else {
        // RTC had nothing, so this is a power event, and the flash checkpoint is
        // the only thing left that knows how long the device had been up. Only
        // consulted here: where RTC survived it is exact and a floor would be a
        // worse answer. load_from_nvs() has already restored s_next_seq, and the
        // increment below has not happened yet, so s_next_seq - 1 is the boot
        // that just ended. On a device with no history at all that is 0, which
        // matches no checkpoint - the first boot has nothing to look up.
        //
        // A floor of 0 is kept, not discarded: it is the seed that boot wrote as
        // it started, and it says the boot ended inside the first ten seconds.
        uint32_t ckpt = 0;
        if (load_checkpoint(s_next_seq - 1, &ckpt)) {
            rec.flags |= RESET_LOG_F_PREV_UPTIME_MIN;
            rec.prev_uptime_s = ckpt;
        }
    }

    if (rail != RESET_RAIL_UNKNOWN) {
        rec.flags |= RESET_LOG_F_RAIL_KNOWN;
        if (rail == RESET_RAIL_HELD) {
            rec.flags |= RESET_LOG_F_RAIL_HELD;
        }
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    strlcpy(rec.version, desc->version, sizeof(rec.version));

    // Both of these read otadata and the partition table off flash directly -
    // no netif, no event loop, nothing this early in app_main is missing.
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        strlcpy(rec.part, running->label, sizeof(rec.part));

        esp_ota_img_states_t st;
        if (esp_ota_get_state_partition(running, &st) == ESP_OK &&
            st == ESP_OTA_IMG_PENDING_VERIFY) {
            // True only because this runs well before the
            // esp_ota_mark_app_valid_cancel_rollback() call later in app_main.
            // Written after that, every successful OTA would look like an
            // ordinary boot and the rollback test below would have nothing to
            // compare against.
            rec.flags |= RESET_LOG_F_OTA_PENDING;
        }

        // ESP_OTA_IMG_ABORTED is sticky: the bootloader stamps it on the slot it
        // gave up on, and it stays there until the next esp_ota_begin()
        // overwrites that slot. Testing it on its own would report "rolled back"
        // on every boot for the rest of the device's life. What makes *this*
        // boot the rollback is that the record immediately before it was a trial
        // image on the other partition - the ring is what dates the sticky
        // marker.
        //
        // Two cases it deliberately does not claim: an empty ring, where there
        // is nothing to date against and a guess would be worse than a blank;
        // and a trial image that died before reaching this function, which never
        // wrote its own pending record, so the revert shows up only as an
        // unexplained partition change.
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        esp_ota_img_states_t other_st;
        if (other != NULL && s_count > 0 &&
            esp_ota_get_state_partition(other, &other_st) == ESP_OK &&
            other_st == ESP_OTA_IMG_ABORTED &&
            (s_ring[s_count - 1].flags & RESET_LOG_F_OTA_PENDING) &&
            strcmp(s_ring[s_count - 1].part, running->label) != 0) {
            rec.flags |= RESET_LOG_F_ROLLBACK;
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    rec.boot_seq = s_next_seq++;
    // What the checkpointer will stamp its writes with, so the next boot can tell
    // this boot's uptime from the one before it. Read without the mutex from
    // reset_log_checkpoint_tick(), which is safe for the same reason the RTC
    // block is: an aligned 32-bit store, published here before any task that
    // could call the tick function exists.
    s_ckpt_boot_seq = rec.boot_seq;
    if (s_count == RESET_LOG_MAX_RECORDS) {
        memmove(&s_ring[0], &s_ring[1], (RESET_LOG_MAX_RECORDS - 1) * sizeof(s_ring[0]));
        s_count--;
    }
    s_ring[s_count++] = rec;
    s_have_current = true;
    save_to_nvs();
    xSemaphoreGive(s_mutex);

    const esp_timer_create_args_t args = {
        .callback = &tick_cb,
        .name = "reset_log_tick",
    };
    esp_err_t err = esp_timer_create(&args, &s_tick_timer);
    if (err != ESP_OK) {
        return err;
    }
    // One second, not ten. The resolution that matters is at the short end:
    // telling "panicked after 2 s" (a boot loop) from "panicked after 40 s"
    // (something the device did) is the entire diagnosis, while 4h12m versus
    // 4h13m is not. Its own timer rather than a hook into sys_monitor's existing
    // 1 Hz loop, because that task starts 35 lines later in app_main and gets
    // starved in exactly the situation this counter exists to measure - a stall
    // ahead of a watchdog reset would freeze the figure and under-report the
    // length of the boot that ended in it.
    err = esp_timer_start_periodic(s_tick_timer, 1000000);
    if (err != ESP_OK) {
        return err;
    }

    char desc_buf[80];
    describe_for_log(&rec, desc_buf, sizeof(desc_buf));
    // Worth an INFO line every boot: log_buf_init() has already run, so this
    // lands in the ring the web log page reads, and after a field reboot it is
    // the single most useful line in it.
    ESP_LOGI(TAG, "last reset: %s (boot #%u recorded)", desc_buf, (unsigned)rec.boot_seq);

    return ESP_OK;
}

void reset_log_note_intent(reset_intent_t intent)
{
    // Guards against a restart requested before reset_log_init() ran, which
    // would otherwise plant a valid-looking intent into a block that has not
    // been re-seeded and attribute the *previous* boot's reset to it.
    if (s_rtc.magic != RESET_LOG_RTC_MAGIC) {
        return;
    }

    // Stopped first, which removes the only overlap between this function and
    // the tick callback, and is right anyway - the device is about to restart.
    // Taking the final sample here rather than letting the last tick stand is
    // what lets a deliberate reboot report its length exactly instead of to the
    // nearest second.
    if (s_tick_timer != NULL) {
        esp_timer_stop(s_tick_timer);
    }
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    s_rtc.uptime_s = up;
    s_rtc.uptime_s_inv = ~up;

    s_rtc.intent = (uint32_t)intent;
    s_rtc.intent_inv = ~(uint32_t)intent;
}

void reset_log_note_ready(void)
{
    if (s_rtc.magic != RESET_LOG_RTC_MAGIC) {
        return;
    }
    s_rtc.reached_ready = 1;
    s_rtc.reached_ready_inv = ~1u;
}

void reset_log_checkpoint_tick(void)
{
    // s_ckpt_boot_seq is set in reset_log_init() under the mutex and is the one
    // thing here that a failed or absent init would leave unset. Without this a
    // device whose init failed would checkpoint against boot #0 forever.
    if (s_ckpt_boot_seq == 0) {
        return;
    }

    // The schedule is finished: this boot has been up more than a year and ">1y"
    // will not become less true. Nothing left to spend flash on.
    if (s_ckpt_next >= CKPT_COUNT) {
        return;
    }

    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    if (up < k_ckpt_times[s_ckpt_next]) {
        return;
    }

    // Steps over every point this tick has already passed, not just the one that
    // came due, so a tick delayed across several of them writes once instead of
    // firing a burst of catch-up writes. Advanced before the write and
    // unconditionally: a write that fails is dropped rather than retried a second
    // later, which on a full or failing NVS partition is the difference between
    // one lost checkpoint and a write storm.
    while (s_ckpt_next < CKPT_COUNT && up >= k_ckpt_times[s_ckpt_next]) {
        s_ckpt_next++;
    }

    // Records what the clock actually says rather than the scheduled value it
    // came due at. A tick that arrives late has measured a longer boot, and ">25s"
    // is a truer statement than the ">20s" the table would have given - the same
    // reason tick_cb() samples esp_timer instead of incrementing a counter.
    save_checkpoint(up);
}

int reset_log_get(reset_log_entry_t *out, int max)
{
    if (s_mutex == NULL) {
        return 0;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = 0;
    // The ring is stored oldest first; every reader wants newest first.
    for (int i = s_count - 1; i >= 0 && n < max; i--) {
        out[n++] = s_ring[i];
    }
    xSemaphoreGive(s_mutex);
    return n;
}

bool reset_log_get_current(reset_log_entry_t *out)
{
    if (s_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool have = s_have_current && s_count > 0;
    if (have) {
        *out = s_ring[s_count - 1];
    }
    xSemaphoreGive(s_mutex);
    return have;
}
