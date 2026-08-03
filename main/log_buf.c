#include <stdio.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "log_buf.h"

typedef struct {
    uint32_t ts_ms;
    char level;                    // 'E' 'W' 'I' 'D' 'V', or '?' if unparsed
    char tag[LOG_BUF_TAG_MAX];
    char msg[LOG_BUF_MSG_MAX];
} log_line_t;

static log_line_t s_lines[LOG_BUF_LINES];

// Total lines ever stored. Sequence numbers are 1-based and equal to this
// counter's value at the time the line was stored, so the ring slot for a
// given seq is simply (seq - 1) % LOG_BUF_LINES and no per-line seq field is
// needed.
static uint32_t s_total;

// The one counter that is *not* covered by s_mutex, and can't be: it counts the
// calls that failed to take the mutex. Two tasks - the two cores each run their
// own - can time out at the same moment, so a plain ++ would be a read-modify-
// write race that loses counts exactly when contention is highest and the
// number matters most. Relaxed ordering is enough; nothing is published through
// it, and the value is only ever read for display.
static _Atomic uint32_t s_missed;

// Guards s_lines/s_total *and* s_staging below. A mutex (not a spinlock) is
// the right primitive here: everything that reaches the esp_log vprintf hook
// is already in task context, since ESP_EARLY_LOGx/ESP_DRAM_LOGx and the panic
// handler go to esp_rom_printf and never come through here.
//
// Nothing in the capture path may call ESP_LOGx - that would re-enter the hook
// and take this same non-recursive mutex twice on one task. The bounded
// timeout below turns that class of mistake into a dropped line and a brief
// stall rather than a hung task.
static SemaphoreHandle_t s_mutex;
#define LOG_BUF_LOCK_TIMEOUT_MS 20

// Formatting scratch for one hook call. Static rather than a stack local
// because this runs on whichever task happened to log - including the system
// event task, which gets only CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE (2304)
// bytes. Charging every logging task a few hundred bytes of stack for a buffer
// used one at a time under the lock would be the wrong trade.
static char s_staging[256];

// One hook call is not one log line. It is for ESP_LOGx under
// CONFIG_LOG_VERSION_1 (esp_log_va() forwards the whole LOG_FORMAT string in a
// single call), but the precompiled WiFi driver blobs do their own formatting
// and hit the hook three times per line:
//
//   1. "\033[0;32mI (1613) wifi: "   prefix, no newline
//   2. "Init dynamic tx buffer num: 32"
//   3. "\033[0m\n"                   colour reset and terminator
//
// Storing a ring entry per call turned every one of those into a prefix-only
// row, an unparsed row, and a blank row. So fragments accumulate here and a
// line is committed only when its terminating newline arrives.
//
// Covered by s_mutex along with s_lines/s_total/s_staging.
static char s_pending[256];
static size_t s_pending_len;

// Which task's line is currently being assembled, so a fragment arriving from
// another task can be recognised as an interruption rather than a continuation.
static TaskHandle_t s_pending_task;

// Level of the line currently being assembled. Only the prefix fragment carries
// it, so it is recorded when a line starts (s_pending_len == 0) and every later
// fragment of that line inherits it - otherwise the serial console would print
// fragments 2 and 3 of a line whose prefix its own threshold had just
// suppressed. Valid only while a line is pending; the initialiser just keeps it
// defined before the first one.
static esp_log_level_t s_pending_level = ESP_LOG_ERROR;

static vprintf_like_t s_orig_vprintf;

// Starts at the compiled-in default rather than at the WARN the console
// eventually asks for. This hook is installed at the top of app_main(), long
// before serial_console_init() runs, and starting at WARN here would silently
// drop the whole INFO-level boot sequence from the serial console - output
// that reached it fine before this module existed.
static esp_log_level_t s_serial_level = CONFIG_LOG_DEFAULT_LEVEL;

// Strips ANSI SGR sequences in place. CONFIG_LOG_COLORS wraps every line
// (LOG_FORMAT in esp_log_format.h), so a formatted line arrives looking like
// "\033[0;32mI (12481) wifi: msg\033[0m\n". Skipping the escapes generically
// rather than trimming known-width prefixes keeps this correct if colors are
// turned off, or if a message carries escapes of its own.
//
// Newlines and trailing whitespace are deliberately left alone: this runs per
// fragment, and the newline is what tells the caller a line is finished.
// Right-trimming here would also eat the space in "I (1613) wifi: ", gluing the
// tag to the message body that arrives in the next fragment.
static void strip_ansi(char *s)
{
    char *w = s;
    for (const char *r = s; *r != '\0'; r++) {
        if (*r == '\033' && r[1] == '[') {
            r += 2;
            // CSI runs until a final byte in the range 0x40-0x7e.
            while (*r != '\0' && (*r < '@' || *r > '~')) {
                r++;
            }
            if (*r == '\0') {
                break;
            }
            continue;
        }
        *w++ = *r;
    }
    *w = '\0';
}

// Right-trim, applied once a whole line has been assembled.
static void trim_trailing(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') {
        s[--n] = '\0';
    }
}

// Matches the standard "L (timestamp) " opening. On success *rest points at the
// first character after it.
static bool match_prefix(const char *line, char *level, uint32_t *ts_ms, const char **rest)
{
    // The explicit empty-string test is load-bearing: strchr() matches the
    // terminating NUL, so strchr("EWIDV", '\0') is non-NULL and the line[1] and
    // line[2] reads below would run past the end of the string.
    if (line[0] == '\0' || strchr("EWIDV", line[0]) == NULL ||
        line[1] != ' ' || line[2] != '(') {
        return false;
    }

    char *end = NULL;
    unsigned long ts = strtoul(line + 3, &end, 10);
    if (end == NULL || end[0] != ')' || end[1] != ' ') {
        return false;
    }

    *level = line[0];
    *ts_ms = (uint32_t)ts;
    *rest = end + 2;
    return true;
}

static esp_log_level_t level_from_letter(char letter)
{
    switch (letter) {
        case 'E': return ESP_LOG_ERROR;
        case 'W': return ESP_LOG_WARN;
        case 'I': return ESP_LOG_INFO;
        case 'D': return ESP_LOG_DEBUG;
        case 'V': return ESP_LOG_VERBOSE;
        // Unparsed, so err towards showing it on the console.
        default:  return ESP_LOG_ERROR;
    }
}

// Splits "I (12481) wifi: station joined" into its parts. Anything that
// doesn't match that shape is kept whole as the message with level '?' -
// losing an unusual line is exactly when the log page is most needed.
static void parse_line(const char *line, log_line_t *out)
{
    out->level = '?';
    out->ts_ms = 0;
    out->tag[0] = '\0';

    const char *rest = line;
    if (match_prefix(line, &out->level, &out->ts_ms, &rest)) {
        // The tag is the run up to the first ':'. The space after it is
        // optional, not part of the separator: ESP_LOGx writes "tag: message",
        // but the WiFi driver blobs write "wifi:message" with no space, so
        // requiring ": " split those lines at whichever later colon happened to
        // have a space after it - "wifi:wifi driver task: 3ffd1a8c" came out
        // tagged "wifi:wifi driver task".
        //
        // The first colon is always the right one, since the tag leads the
        // line. The length bound is what stops a line that only looked like it
        // had a prefix from yielding an absurd tag.
        const char *sep = strchr(rest, ':');
        if (sep != NULL && (size_t)(sep - rest) < LOG_BUF_TAG_MAX) {
            size_t taglen = (size_t)(sep - rest);
            memcpy(out->tag, rest, taglen);
            out->tag[taglen] = '\0';
            rest = sep + 1;
            if (*rest == ' ') {
                rest++;
            }
        }
    }

    strlcpy(out->msg, rest, sizeof(out->msg));
}

// Stores the assembled line and starts a new one. Callers hold s_mutex.
//
// An empty line is dropped rather than stored. The colour-reset fragment that
// terminates a WiFi driver line strips down to nothing at all, and a ring full
// of blank rows is exactly what this module used to serve the log page.
static void commit_pending(void)
{
    s_pending[s_pending_len] = '\0';
    trim_trailing(s_pending);

    if (s_pending[0] != '\0') {
        parse_line(s_pending, &s_lines[s_total % LOG_BUF_LINES]);
        s_total++;
    }

    s_pending_len = 0;
}

// Commits early when the buffer fills, so a line that never terminates costs an
// extra entry instead of wedging the accumulator or dropping its tail.
//
// Note this leaves s_pending_len non-zero, so the fragments that follow are
// still read as a continuation and keep the line's level. That is why
// commit_pending() does not reset s_pending_level: doing so demoted the tail of
// an over-long line to an unparsed fragment, and unparsed means ESP_LOG_ERROR -
// the console would print the remainder of an INFO line its own threshold had
// suppressed the head of. The level is instead (re)established below, on the
// one path that genuinely starts a new line.
static void pending_putc(char c)
{
    if (s_pending_len + 1 >= sizeof(s_pending)) {
        commit_pending();
    }
    s_pending[s_pending_len++] = c;
}

// Returns the level of the line this fragment belongs to, so the caller can
// decide whether it also belongs on the serial console.
static esp_log_level_t capture(const char *fmt, va_list ap)
{
    if (s_mutex == NULL) {
        return ESP_LOG_ERROR;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(LOG_BUF_LOCK_TIMEOUT_MS)) != pdTRUE) {
        atomic_fetch_add_explicit(&s_missed, 1, memory_order_relaxed);
        // Unparsed, so err towards showing it on the console.
        return ESP_LOG_ERROR;
    }

    vsnprintf(s_staging, sizeof(s_staging), fmt, ap);
    strip_ansi(s_staging);

    char letter = '?';
    uint32_t ts_ms = 0;
    const char *rest = NULL;
    bool opens_line = match_prefix(s_staging, &letter, &ts_ms, &rest);

    // A line is assembled over several calls, but the mutex is only held for
    // one call at a time - so another task can log in the middle of one. Two
    // things say the pending line has been interrupted: the fragment comes from
    // a different task, or it opens a prefixed line of its own. Either way,
    // close out what is pending before starting on it. Without this the two get
    // spliced: a WiFi "<ba-add>" line came out of the ring with a client_track
    // warning welded onto its tail and its own tail truncated away.
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (s_pending_len > 0 && (self != s_pending_task || opens_line)) {
        commit_pending();
    }

    // A fragment that opens a line carries the level for the whole of it.
    if (s_pending_len == 0) {
        s_pending_task = self;
        s_pending_level = opens_line ? level_from_letter(letter) : ESP_LOG_ERROR;
    }
    // Snapshotted before the walk below, which commits the line and resets it.
    esp_log_level_t level = s_pending_level;

    // Everything up to and including the fragment's last newline closes out the
    // current line; anything after it opens the next one. Interior newlines are
    // folded to spaces rather than split on, because a single ESP_LOGx call can
    // embed them - main.c prints its banners that way - and the ring holds one
    // entry per logical line.
    const char *last_nl = strrchr(s_staging, '\n');
    for (const char *r = s_staging; *r != '\0'; r++) {
        if (r == last_nl) {
            commit_pending();
            continue;
        }
        pending_putc((*r == '\n' || *r == '\r') ? ' ' : *r);
    }

    xSemaphoreGive(s_mutex);
    return level;
}

static int log_vprintf(const char *fmt, va_list ap)
{
    // capture() consumes its va_list, and the serial path needs an untouched
    // one. Forwarding the original arguments (rather than the string capture()
    // already built) keeps serial output byte-identical to before this hook
    // existed - colors, and no truncation at s_staging's size.
    va_list serial_ap;
    va_copy(serial_ap, ap);

    esp_log_level_t level = capture(fmt, ap);

    int n = 0;
    if (level <= s_serial_level) {
        n = s_orig_vprintf(fmt, serial_ap);
    }
    va_end(serial_ap);
    return n;
}

void log_buf_init(void)
{
    if (s_mutex != NULL) {
        return;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return;  // Can't ESP_LOGE here: the hook isn't installed, but a failed
                 // mutex means the whole module stays inert either way.
    }
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf);
}

void log_buf_set_serial_level(esp_log_level_t level)
{
    s_serial_level = level;
}

esp_log_level_t log_buf_get_serial_level(void)
{
    return s_serial_level;
}

void log_buf_get_range(uint32_t *oldest, uint32_t *newest, uint32_t *missed)
{
    if (s_mutex == NULL) {
        *oldest = 1;
        *newest = 0;
        *missed = 0;
        return;
    }
    // s_missed is read outside the lock because it lives outside it - the mutex
    // says nothing about a counter incremented by the tasks that failed to take
    // it.
    *missed = atomic_load_explicit(&s_missed, memory_order_relaxed);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *newest = s_total;
    *oldest = (s_total > LOG_BUF_LINES) ? (s_total - LOG_BUF_LINES + 1) : 1;
    xSemaphoreGive(s_mutex);
}

bool log_buf_get_line(uint32_t seq, char *level, uint32_t *ts_ms,
                      char *tag, size_t tagsz, char *msg, size_t msgsz)
{
    if (s_mutex == NULL || seq == 0) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    bool ok = seq <= s_total && (s_total - seq) < LOG_BUF_LINES;
    if (ok) {
        const log_line_t *slot = &s_lines[(seq - 1) % LOG_BUF_LINES];
        *level = slot->level;
        *ts_ms = slot->ts_ms;
        strlcpy(tag, slot->tag, tagsz);
        strlcpy(msg, slot->msg, msgsz);
    }

    xSemaphoreGive(s_mutex);
    return ok;
}
