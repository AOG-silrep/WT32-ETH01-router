#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "web_server.h"
#include "wifi_cfg.h"
#include "auth_cfg.h"
#include "client_track.h"
#include "sys_monitor.h"
#include "log_buf.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web_server";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t admin_html_start[] asm("_binary_admin_html_start");
extern const uint8_t admin_html_end[] asm("_binary_admin_html_end");
extern const uint8_t logs_html_start[] asm("_binary_logs_html_start");
extern const uint8_t logs_html_end[] asm("_binary_logs_html_end");

// Pulls a JSON string field's value out of a flat {"key":"value",...}
// object. Good enough for the small, fixed-shape request bodies this
// server accepts - not a general JSON parser.
static bool json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return false;
    }
    p = strchr(p + strlen(pattern), ':');
    if (p == NULL) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;

    size_t i = 0;
    while (*p != '\0' && *p != '"' && i < out_len - 1) {
        if (*p == '\\' && *(p + 1) != '\0') {
            p++;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

// Pulls a JSON numeric field's value out of a flat {"key":123,...} object.
static bool json_get_int(const char *json, const char *key, long *out)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return false;
    }
    p = strchr(p + strlen(pattern), ':');
    if (p == NULL) {
        return false;
    }
    p++;
    char *end;
    long val = strtol(p, &end, 10);
    if (end == p) {
        return false;
    }
    *out = val;
    return true;
}

static int base64_decode_char(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Decodes a standard base64 string (RFC 4648, '+'/'/' alphabet, '='
// padding) - just enough to unpack the "Authorization: Basic ..." header,
// not a general-purpose decoder.
static bool base64_decode(const char *in, size_t in_len, char *out, size_t out_cap, size_t *out_len)
{
    if (in_len == 0 || in_len % 4 != 0) {
        return false;
    }
    size_t written = 0;
    size_t num_groups = in_len / 4;
    for (size_t g = 0; g < num_groups; g++) {
        size_t i = g * 4;
        int vals[4];
        int pad = 0;
        bool seen_pad = false;
        for (int j = 0; j < 4; j++) {
            char c = in[i + j];
            if (c == '=') {
                seen_pad = true;
                vals[j] = 0;
                pad++;
            } else {
                if (seen_pad) {
                    return false;
                }
                vals[j] = base64_decode_char(c);
                if (vals[j] < 0) {
                    return false;
                }
            }
        }
        if (pad > 2 || (pad > 0 && g != num_groups - 1)) {
            return false;
        }
        unsigned char b0 = (unsigned char)((vals[0] << 2) | (vals[1] >> 4));
        unsigned char b1 = (unsigned char)((vals[1] << 4) | (vals[2] >> 2));
        unsigned char b2 = (unsigned char)((vals[2] << 6) | vals[3]);

        int out_bytes = 3 - pad;
        if (out_bytes >= 1) {
            if (written >= out_cap) return false;
            out[written++] = (char)b0;
        }
        if (out_bytes >= 2) {
            if (written >= out_cap) return false;
            out[written++] = (char)b1;
        }
        if (out_bytes >= 3) {
            if (written >= out_cap) return false;
            out[written++] = (char)b2;
        }
    }
    *out_len = written;
    return true;
}

// Checks the "Authorization: Basic ..." header against the saved admin
// credentials. Using standard HTTP Basic Auth (rather than a bespoke
// header/cookie scheme) means the browser itself prompts for and caches
// the credentials, so every route - including the page itself - can be
// gated with no client-side login code: the very first request for "/"
// gets a 401 challenge, the browser's native dialog collects
// username/password, and every request after that (including the JS
// polling fetch() calls) automatically carries the Authorization header.
static bool check_admin_auth(httpd_req_t *req)
{
    char hdr[160]; // "Basic " + base64(longest possible "user:password")
    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdr_len == 0 || hdr_len >= sizeof(hdr)) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }

    const char *prefix = "Basic ";
    size_t prefix_len = strlen(prefix);
    if (strncmp(hdr, prefix, prefix_len) != 0) {
        return false;
    }

    char decoded[AUTH_CFG_USERNAME_MAX_LEN + AUTH_CFG_PASSWORD_MAX_LEN];
    size_t decoded_len = 0;
    if (!base64_decode(hdr + prefix_len, hdr_len - prefix_len, decoded, sizeof(decoded) - 1, &decoded_len)) {
        return false;
    }
    decoded[decoded_len] = '\0';

    char *colon = strchr(decoded, ':');
    if (colon == NULL) {
        return false;
    }
    *colon = '\0';
    const char *user = decoded;
    const char *password = colon + 1;

    char expected_user[AUTH_CFG_USERNAME_MAX_LEN];
    char expected_password[AUTH_CFG_PASSWORD_MAX_LEN];
    auth_cfg_load(expected_user, expected_password);

    if (strcmp(user, expected_user) != 0) {
        return false;
    }
    return strcmp(password, expected_password) == 0;
}

// Throttles credential guessing: every failed attempt costs a full second,
// capping a brute-force run at ~1 guess/sec instead of as fast as the server
// can answer. The httpd runs a single worker task, so this parks the whole
// server for that second - acceptable only because the UI's polling loop
// stops on 401 rather than free-running at 1Hz (see index.html's poll()).
#define AUTH_FAIL_DELAY_MS 1000

// True for a browser navigating to a page, false for fetch()/curl/scripts,
// which send "*/*". Browsers list text/html first, so a truncated read still
// answers the question - strlcpy() null-terminates whatever fit.
static bool client_wants_html(httpd_req_t *req)
{
    char accept[48];
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Accept", accept, sizeof(accept));
    if (err != ESP_OK && err != ESP_ERR_HTTPD_RESULT_TRUNC) {
        return false;
    }
    return strstr(accept, "text/html") != NULL;
}

// Sent when the browser's sign-in dialog is dismissed, or when a stale tab
// navigates here. Without it the address bar would land on raw JSON, which
// tells a person nothing about what went wrong.
static const char auth_error_html[] =
    "<!doctype html><meta charset=\"utf-8\"><title>Sign in &ndash; AgOpen router</title>"
    "<body style=\"font-family:sans-serif;max-width:420px;margin:2em auto;padding:0 1em;color:#222\">"
    "<h1 style=\"font-size:1.3em\">Sign in required</h1>"
    "<p>The bridge didn't accept those credentials. This usually means the admin "
    "password was changed or reset since this page was opened.</p>"
    "<p><a href=\"/\">Try again</a></p>";

// The rejection line below is the only thing an *unauthenticated* caller can
// put in the log ring, and AUTH_FAIL_DELAY_MS sets its pace: one line per
// second, which is LOG_BUF_LINES (128) - the whole ring - in a little over two
// minutes. So a bare `while :; do curl -s http://bridge/api/status; done`
// quietly erases every line describing what the device was actually doing,
// which is exactly the history worth having while someone is probing the box.
//
// At most one line therefore goes out per window; the rest are counted and
// reported on the next one. A probe still shows up in the log - it just costs a
// line a minute instead of a line a second, leaving the ring holding hours of
// everything else.
#define AUTH_FAIL_LOG_INTERVAL_MS 60000

// How much of the URI that line carries. The caller chooses this string, so its
// length is capped here rather than left to fill LOG_BUF_MSG_MAX (144) and push
// the rest of the message - the suppressed count included - off the end. The
// path leads the URI, so the head is the part that says what was probed.
#define AUTH_FAIL_LOG_URI_MAX 48

// Unguarded on purpose: esp_http_server dispatches every handler from its one
// worker task - the same property AUTH_FAIL_DELAY_MS above leans on - so these
// are only ever touched from a single task.
static uint32_t s_auth_fail_unreported;
static TickType_t s_auth_fail_last_log;
static bool s_auth_fail_logged;

static void log_auth_failure(httpd_req_t *req)
{
    TickType_t now = xTaskGetTickCount();
    s_auth_fail_unreported++;

    // Unsigned tick subtraction, so the window stays correct across the
    // TickType_t rollover. The flag is what makes the first rejection after
    // boot print: s_auth_fail_last_log starts at 0, which within the first
    // minute of uptime reads as "just logged".
    if (s_auth_fail_logged &&
        (TickType_t)(now - s_auth_fail_last_log) < pdMS_TO_TICKS(AUTH_FAIL_LOG_INTERVAL_MS)) {
        return;
    }

    uint32_t also = s_auth_fail_unreported - 1;
    s_auth_fail_unreported = 0;
    s_auth_fail_last_log = now;
    s_auth_fail_logged = true;

    if (also == 0) {
        ESP_LOGW(TAG, "Rejected request for %.*s: bad or missing credentials",
                 AUTH_FAIL_LOG_URI_MAX, req->uri);
        return;
    }
    // "since the last of these" rather than "in the last minute": the count is
    // carried until there is a line to print it on, so a burst that stops
    // mid-window is still reported by whatever rejection comes next, however
    // much later that is, instead of being dropped for want of an occasion.
    ESP_LOGW(TAG, "Rejected request for %.*s: bad or missing credentials "
                  "(%u more since the last of these)",
             AUTH_FAIL_LOG_URI_MAX, req->uri, (unsigned)also);
}

static void send_auth_error(httpd_req_t *req)
{
    vTaskDelay(pdMS_TO_TICKS(AUTH_FAIL_DELAY_MS));
    log_auth_failure(req);

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"AgOpen router\"");
    if (client_wants_html(req)) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, auth_error_html, HTTPD_RESP_USE_STRLEN);
        return;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"Authentication required\"}", HTTPD_RESP_USE_STRLEN);
}

static void send_default_creds_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/admin?forced=1");
    httpd_resp_send(req, NULL, 0);
}

// The API half of the same gate the redirect above applies to "/": while the
// admin password is the compiled-in default, the device is unconfigured and
// answers nothing but the two routes needed to configure it. Otherwise the
// forced change would be cosmetic - a script or curl never requests "/", so it
// would never see the redirect and would drive the whole API, OTA included, on
// credentials everyone already knows.
//
// No AUTH_FAIL_DELAY_MS here, unlike send_auth_error() next door: the caller
// authenticated fine, so this isn't a guess to throttle, and the httpd's single
// worker task must not be parked for a second on a legitimate request.
static void send_setup_required(httpd_req_t *req)
{
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"Default admin password in use - set a new one at /admin\"}",
                     HTTPD_RESP_USE_STRLEN);
}

// Appends a formatted string to buf at offset off, like the
// `off += snprintf(resp + off, sizeof(resp) - off, ...)` pattern this
// replaces, but clamps the returned offset to at most bufsz. Plain
// snprintf() returns the length that *would* have been written had the
// buffer been big enough - if a call truncates, chaining that raw return
// value into the next call's `bufsz - off` underflows (size_t) and turns
// `buf + off` into an out-of-bounds pointer. Routing every accumulation
// through here means off <= bufsz holds after every single call, so
// `bufsz - off` can never wrap, no matter how many fields get appended.
//
// The clamp doubles as the truncation signal: off only ever reaches bufsz by
// being clamped there, so `off >= bufsz` after the last append means the
// response did not fit. resp_send_json() below is what tests it.
//
// Call this through the resp_append() macro, which supplies `who` from
// __func__ so the warning names the handler that overflowed.
static int __attribute__((format(printf, 5, 6)))
resp_append_at(const char *who, char *buf, size_t bufsz, int off, const char *fmt, ...)
{
    if (off < 0 || (size_t)off > bufsz) {
        return (int)bufsz;
    }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, bufsz - off, fmt, ap);
    va_end(ap);

    if (n < 0) {
        return off;
    }

    size_t new_off = (size_t)off + (size_t)n;

    // n == bufsz - off already means one character was dropped to make room
    // for the NUL, hence >= rather than >. Requiring off < bufsz on entry
    // limits this to the one call that fills the buffer, so a response gets
    // one warning instead of one per remaining append.
    //
    // Deliberately not rate-limited beyond that. A repeating overflow already
    // puts two lines per second on the console from esp_http_server itself
    // (the 500 from resp_send_json(), then "uri handler execution failed"),
    // and those say nothing about which handler or which buffer. Throttling
    // this one only strips the diagnosis out of a flood that happens anyway.
    // One warning per failing response keeps it next to the 500 it explains.
    if ((size_t)off < bufsz && new_off >= bufsz) {
        // "at least" because the appends that follow this one are never
        // measured - the real requirement is only ever larger.
        ESP_LOGW(TAG, "%s: response truncated - %u-byte buffer needs at least %u",
                 who, (unsigned)bufsz, (unsigned)(new_off + 1));
    }

    return (int)(new_off > bufsz ? bufsz : new_off);
}

#define resp_append(buf, bufsz, off, ...) \
    resp_append_at(__func__, (buf), (bufsz), (off), __VA_ARGS__)

// Appends src as the body of a JSON string (the surrounding quotes are the
// caller's), escaping the three things that would otherwise produce a document
// no parser will accept: '"', '\', and raw control characters.
//
// Every string this server emits is device-controlled but not device-authored -
// log messages, DHCP hostnames, the SSID - so "in practice these never contain
// a quote" is an assumption about other people's input, not an invariant. When
// it breaks, the dashboard's JSON.parse() throws and the whole page goes blank
// with nothing on screen to explain why.
//
// Follows resp_append_at()'s contract: returns an offset clamped to bufsz, so
// chaining into `bufsz - off` can never underflow. Truncation is silent here
// because the resp_append() that closes the field will report it.
static int json_append_escaped(char *buf, size_t bufsz, int off, const char *src)
{
    // >= bufsz, not > bufsz: an earlier append that filled the buffer returns
    // exactly bufsz, and the empty-string path below still writes a NUL at
    // buf[off]. At off == bufsz that is one byte past the end.
    if (off < 0 || (size_t)off >= bufsz) {
        return (int)bufsz;
    }
    size_t o = (size_t)off;
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
        // Worst case below is 6 bytes plus the NUL that terminates the buffer
        // for the appends that follow.
        if (o + 7 > bufsz) {
            return (int)bufsz;
        }
        switch (*p) {
            case '"':  buf[o++] = '\\'; buf[o++] = '"';  break;
            case '\\': buf[o++] = '\\'; buf[o++] = '\\'; break;
            case '\n': buf[o++] = '\\'; buf[o++] = 'n';  break;
            case '\r': buf[o++] = '\\'; buf[o++] = 'r';  break;
            case '\t': buf[o++] = '\\'; buf[o++] = 't';  break;
            default:
                if (*p < 0x20) {
                    o += (size_t)snprintf(buf + o, bufsz - o, "\\u%04x", *p);
                } else {
                    buf[o++] = (char)*p;
                }
                break;
        }
    }
    buf[o] = '\0';
    return (int)o;
}

// Sends a JSON body built by resp_append(), or fails the request if it was
// truncated. Sending the truncated bytes instead would hand the dashboard
// unparseable JSON, which surfaces as an empty table with nothing to explain
// it; resp_append_at() has already logged the buffer and the size it needed.
// Returns false if the caller should return ESP_FAIL.
static bool resp_send_json(httpd_req_t *req, const char *resp, int off, size_t bufsz)
{
    if (off < 0 || (size_t)off >= bufsz) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Response buffer overflow");
        return false;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, off);
    return true;
}

// How many idle stretches a body may span. httpd_req_recv() returns what one
// socket read produced, not what was asked for, and a gap longer than
// recv_wait_timeout (5s) between segments comes back as a timeout rather than an
// error - retrying is what makes a body split across TCP segments arrive whole.
// Bounded, though: the httpd has one worker task, so retrying forever would let
// any client stall every other request for as long as it cared to dribble a body
// out.
#define RECV_BODY_MAX_TIMEOUTS 3

// Reads the whole request body into buf and NUL-terminates it, returning its
// length, or -1 having already answered the caller.
//
// The single httpd_req_recv() this replaces stopped at whatever the first read
// returned. Short of content_len that leaves truncated JSON, which
// json_get_string() then reports as a missing field - a transport hiccup blamed
// on the request. On /api/admin it was worse than a bad message: the cut can
// fall between the two fields, saving a new username against the old password
// while the response says the request was rejected.
static int recv_body(httpd_req_t *req, char *buf, size_t bufsz)
{
    // content_len is unsigned, so this is the "missing" half of the message.
    if (req->content_len == 0 || req->content_len >= bufsz) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body missing or too large");
        return -1;
    }

    size_t total = 0;
    int timeouts = 0;
    while (total < req->content_len) {
        int received = httpd_req_recv(req, buf + total, req->content_len - total);
        if (received > 0) {
            total += (size_t)received;
            continue;
        }
        if (received == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= RECV_BODY_MAX_TIMEOUTS) {
            continue;
        }
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
        return -1;
    }
    buf[total] = '\0';
    return (int)total;
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_default_creds_redirect(req);
        return ESP_OK;
    }

    // no-store, not just no-cache: these pages are only served to an
    // authenticated caller, and a cached copy would still render after the
    // credentials behind it stopped working - so a reload meant to re-trigger
    // the sign-in dialog would come off the disk cache and never reach us.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start,
                     index_html_end - index_html_start - 1);
    return ESP_OK;
}

static esp_err_t admin_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)admin_html_start,
                     admin_html_end - admin_html_start - 1);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_setup_required(req);
        return ESP_OK;
    }

    wifi_config_t cfg;
    esp_wifi_get_config(WIFI_IF_AP, &cfg);

    // The SSID is user-supplied text, and escaping can grow it up to 6x
    // (a control character becomes "\u00xx"), so size for the worst case
    // rather than for what a reasonable SSID looks like.
    char resp[6 * 32 + 48];
    int len = resp_append(resp, sizeof(resp), 0, "{\"ssid\":\"");
    len = json_append_escaped(resp, sizeof(resp), len, (const char *)cfg.ap.ssid);
    len = resp_append(resp, sizeof(resp), len, "\",\"channel\":%u}", (unsigned)cfg.ap.channel);
    if (!resp_send_json(req, resp, len, sizeof(resp))) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_setup_required(req);
        return ESP_OK;
    }

    char buf[256];
    if (recv_body(req, buf, sizeof(buf)) < 0) {
        return ESP_FAIL;
    }

    char ssid[WIFI_CFG_SSID_MAX_LEN] = {0};
    char password[WIFI_CFG_PASSWORD_MAX_LEN] = {0};
    if (!json_get_string(buf, "ssid", ssid, sizeof(ssid))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing \"ssid\"");
        return ESP_FAIL;
    }
    json_get_string(buf, "password", password, sizeof(password)); // absent/blank -> keep current password
    if (password[0] == '\0') {
        char cur_ssid[WIFI_CFG_SSID_MAX_LEN];
        uint8_t cur_channel;
        wifi_cfg_load(cur_ssid, password, &cur_channel);
    }

    long channel_val = WIFI_CFG_DEFAULT_CHANNEL;
    json_get_int(buf, "channel", &channel_val); // absent -> default channel
    uint8_t channel = (channel_val < 0 || channel_val > 255) ? 0 : (uint8_t)channel_val;

    const char *err_msg = NULL;
    if (!wifi_cfg_validate(ssid, password, channel, &err_msg)) {
        char resp[128];
        int len = resp_append(resp, sizeof(resp), 0, "{\"ok\":false,\"error\":\"%s\"}", err_msg);
        httpd_resp_set_status(req, "400 Bad Request");
        if (!resp_send_json(req, resp, len, sizeof(resp))) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    if (wifi_cfg_save(ssid, password, channel) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Failed to save\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);

    // Reboot rather than reconfiguring the AP in place: the WiFi AP netif is
    // a bridge port wired into esp_netif_br_glue, and hot-applying a new
    // config via esp_wifi_set_config() leaves the bridge broken until a
    // manual power-cycle. A full restart re-applies the saved config
    // cleanly via the normal boot path (wifi_init_softap()). Delay briefly
    // so the response above actually reaches the client first.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t admin_post_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }

    char buf[256];
    if (recv_body(req, buf, sizeof(buf)) < 0) {
        return ESP_FAIL;
    }

    char new_user[AUTH_CFG_USERNAME_MAX_LEN];
    char new_password[AUTH_CFG_PASSWORD_MAX_LEN];
    auth_cfg_load(new_user, new_password); // start from current values - either field may be left unchanged

    char tmp_user[AUTH_CFG_USERNAME_MAX_LEN] = {0};
    char tmp_password[AUTH_CFG_PASSWORD_MAX_LEN] = {0};
    if (json_get_string(buf, "new_admin_user", tmp_user, sizeof(tmp_user)) && tmp_user[0] != '\0') {
        strcpy(new_user, tmp_user);
    }
    if (json_get_string(buf, "new_admin_password", tmp_password, sizeof(tmp_password)) && tmp_password[0] != '\0') {
        strcpy(new_password, tmp_password);
    }

    const char *err_msg = NULL;
    if (!auth_cfg_validate(new_user, new_password, &err_msg)) {
        char resp[128];
        int len = resp_append(resp, sizeof(resp), 0, "{\"ok\":false,\"error\":\"%s\"}", err_msg);
        httpd_resp_set_status(req, "400 Bad Request");
        if (!resp_send_json(req, resp, len, sizeof(resp))) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    if (auth_cfg_save(new_user, new_password) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Failed to save\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // No reboot needed - check_admin_auth() reloads credentials from NVS on
    // every request, so the new pair takes effect on the very next one. The
    // browser's cached Basic Auth credentials for this session are now
    // stale and it'll be re-challenged next time it hits any endpoint.
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Static, not stack-allocated: the httpd worker task's stack is shared across
// all handlers, and a 4KB local buffer would blow it.
static uint8_t ota_buf[4096];

// Drain the body ourselves in large chunks after rejecting an upload. Left to
// the framework, the post-handler purge reads CONFIG_HTTPD_PURGE_BUF_LEN (32)
// bytes at a time against a multi-megabyte image; any inter-chunk gap past
// recv_wait_timeout (5s) makes it bail and force-close the socket mid-upload,
// which surfaces to the client as a connection reset instead of the error
// response it had already received.
static void ota_drain_body(httpd_req_t *req)
{
    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining < sizeof(ota_buf) ? remaining : sizeof(ota_buf);
        int received = httpd_req_recv(req, (char *)ota_buf, to_read);
        if (received <= 0) {
            break;
        }
        remaining -= received;
    }
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        ota_drain_body(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_setup_required(req);
        ota_drain_body(req);
        return ESP_OK;
    }

    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body missing");
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition available");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, req->content_len, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Image too large or OTA busy");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining < sizeof(ota_buf) ? remaining : sizeof(ota_buf);
        int received = httpd_req_recv(req, (char *)ota_buf, to_read);
        if (received <= 0) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, ota_buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write failed");
            return ESP_FAIL;
        }
        remaining -= received;
    }

    // esp_ota_end() validates the image header/checksum - on failure the
    // partition just written is left un-booted, so a corrupt upload can't
    // brick the device; it just doesn't take effect.
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        char resp[128];
        int len = resp_append(resp, sizeof(resp), 0,
                              "{\"ok\":false,\"error\":\"Image validation failed: %s\"}",
                              esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        if (!resp_send_json(req, resp, len, sizeof(resp))) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Failed to set boot partition\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);

    // Same pattern as wifi_post_handler: let the response reach the client
    // before tearing everything down for the reboot into the new image.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t clients_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_setup_required(req);
        return ESP_OK;
    }

    client_info_t clients[CLIENT_TRACK_MAX_CLIENTS];
    int count = 0;
    client_track_get_snapshot(clients, CLIENT_TRACK_MAX_CLIENTS, &count);

    // 6x the name length: JSON-escaping a control character expands it to
    // "\u00xx", and the hostname is the one field here a client controls.
    static char resp[CLIENT_TRACK_MAX_CLIENTS * (200 + 6 * CLIENT_TRACK_NAME_MAX_LEN) + 16];
    int off = resp_append(resp, sizeof(resp), 0, "[");
    for (int i = 0; i < count && off < sizeof(resp); i++) {
        client_info_t *c = &clients[i];
        char ip_str[16];
        esp_ip4addr_ntoa(&c->ip, ip_str, sizeof(ip_str));
        char rssi_str[8];
        if (c->is_wifi) {
            snprintf(rssi_str, sizeof(rssi_str), "%d", c->rssi);
        } else {
            strcpy(rssi_str, "null");
        }
        off = resp_append(resp, sizeof(resp), off,
                           "%s{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"ip\":\"%s\","
                           "\"link\":\"%s\",\"rssi\":%s,\"rx_bps\":%u,\"tx_bps\":%u,"
                           "\"rx_pps\":%u,\"tx_pps\":%u,"
                           "\"last_seen_s\":%u,\"name\":\"",
                           i == 0 ? "" : ",",
                           c->mac[0], c->mac[1], c->mac[2], c->mac[3], c->mac[4], c->mac[5],
                           ip_str, c->is_wifi ? "wifi" : "eth", rssi_str,
                           (unsigned)c->rx_bps, (unsigned)c->tx_bps,
                           (unsigned)c->rx_pps, (unsigned)c->tx_pps, (unsigned)c->last_seen_s);
        // A DHCP hostname is whatever the client claimed it is - the one field
        // here that an untrusted device on the network gets to choose.
        off = json_append_escaped(resp, sizeof(resp), off, c->name);
        off = resp_append(resp, sizeof(resp), off, "\"}");
    }
    off = resp_append(resp, sizeof(resp), off, "]");

    if (!resp_send_json(req, resp, off, sizeof(resp))) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Parses "aa:bb:cc:dd:ee:ff" into 6 raw bytes. Uses an unsigned int scratch
// array rather than scanning directly into mac's uint8_t slots - %hhx support
// varies across newlib scanf configurations, %x into a wider int does not.
static bool parse_mac(const char *str, uint8_t mac[6])
{
    unsigned int b[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)b[i];
    }
    return true;
}

static esp_err_t client_history_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_setup_required(req);
        return ESP_OK;
    }

    char query[128];
    char mac_str[24] = {0};
    char since_str[16] = {0};
    // Same split as logs_get_handler(): a query too long for the buffer comes
    // back as ESP_ERR_HTTPD_RESULT_TRUNC with nothing copied, so folding it in
    // with ESP_ERR_NOT_FOUND would blame the caller's "mac" for a length
    // problem it doesn't have.
    esp_err_t query_err = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (query_err == ESP_ERR_HTTPD_RESULT_TRUNC) {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "Query string too long");
        return ESP_FAIL;
    }
    if (query_err == ESP_OK) {
        httpd_query_key_value(query, "mac", mac_str, sizeof(mac_str));
        httpd_query_key_value(query, "since", since_str, sizeof(since_str));
    }

    uint8_t mac[6];
    if (!parse_mac(mac_str, mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid \"mac\"");
        return ESP_FAIL;
    }
    uint32_t since_seq = since_str[0] ? (uint32_t)strtoul(since_str, NULL, 10) : 0;

    client_history_t hist;
    if (!client_track_get_history(mac, since_seq, &hist)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown client");
        return ESP_FAIL;
    }

    static char resp[1600];
    int off = resp_append(resp, sizeof(resp), 0,
                           "{\"mac\":\"%s\",\"period_ms\":%d,\"seq\":%u,\"reset\":%s,\"rx\":[",
                           mac_str, CLIENT_TRACK_HISTORY_PERIOD_MS, (unsigned)hist.seq,
                           hist.reset ? "true" : "false");
    for (int i = 0; i < hist.count && off < sizeof(resp); i++) {
        off = resp_append(resp, sizeof(resp), off, "%s%u", i == 0 ? "" : ",", (unsigned)hist.rx_bps[i]);
    }
    off = resp_append(resp, sizeof(resp), off, "],\"tx\":[");
    for (int i = 0; i < hist.count && off < sizeof(resp); i++) {
        off = resp_append(resp, sizeof(resp), off, "%s%u", i == 0 ? "" : ",", (unsigned)hist.tx_bps[i]);
    }
    off = resp_append(resp, sizeof(resp), off, "],\"rx_pps\":[");
    for (int i = 0; i < hist.count && off < sizeof(resp); i++) {
        off = resp_append(resp, sizeof(resp), off, "%s%u", i == 0 ? "" : ",", (unsigned)hist.rx_pps[i]);
    }
    off = resp_append(resp, sizeof(resp), off, "],\"tx_pps\":[");
    for (int i = 0; i < hist.count && off < sizeof(resp); i++) {
        off = resp_append(resp, sizeof(resp), off, "%s%u", i == 0 ? "" : ",", (unsigned)hist.tx_pps[i]);
    }
    off = resp_append(resp, sizeof(resp), off, "]}");

    if (!resp_send_json(req, resp, off, sizeof(resp))) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t system_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_setup_required(req);
        return ESP_OK;
    }

    uint8_t cpu_pct[SYS_MONITOR_NUM_CORES];
    sys_monitor_get_cpu_load(cpu_pct);

    client_info_t clients[CLIENT_TRACK_MAX_CLIENTS];
    int count = 0;
    client_track_get_snapshot(clients, CLIENT_TRACK_MAX_CLIENTS, &count);
    uint32_t rx_total = 0, tx_total = 0;
    for (int i = 0; i < count; i++) {
        rx_total += clients[i].rx_bps;
        tx_total += clients[i].tx_bps;
    }
    uint32_t rx_pps = 0, tx_pps = 0;
    client_track_get_traffic_pps(&rx_pps, &tx_pps);

    const esp_app_desc_t *app_desc = esp_app_get_description();

    char resp[400];
    int len = resp_append(resp, sizeof(resp), 0,
                          "{\"uptime_s\":%llu,\"free_heap\":%u,\"min_free_heap\":%u,"
                          "\"cpu_pct\":[%u,%u],\"cpu_freq_mhz\":%u,\"net_rx_bps\":%u,\"net_tx_bps\":%u,"
                          "\"net_rx_pps\":%u,\"net_tx_pps\":%u,"
                          "\"traffic_drops\":%u,\"version\":\"%s\"}",
                          (unsigned long long)(esp_timer_get_time() / 1000000ULL),
                          (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
                          (unsigned)cpu_pct[0], (unsigned)cpu_pct[1], (unsigned)sys_monitor_get_cpu_freq_mhz(),
                          (unsigned)rx_total, (unsigned)tx_total, (unsigned)rx_pps, (unsigned)tx_pps,
                          (unsigned)client_track_get_traffic_drops(), app_desc->version);
    if (!resp_send_json(req, resp, len, sizeof(resp))) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Level names shared by the log API, matching the ones the "loglevel" console
// command accepts so the two interfaces speak the same vocabulary.
static const struct {
    const char *name;
    esp_log_level_t level;
} log_level_names[] = {
    {"none", ESP_LOG_NONE}, {"error", ESP_LOG_ERROR}, {"warn", ESP_LOG_WARN},
    {"info", ESP_LOG_INFO}, {"debug", ESP_LOG_DEBUG}, {"verbose", ESP_LOG_VERBOSE},
};

static const char *log_level_name(esp_log_level_t level)
{
    for (size_t i = 0; i < sizeof(log_level_names) / sizeof(log_level_names[0]); i++) {
        if (log_level_names[i].level == level) {
            return log_level_names[i].name;
        }
    }
    return "unknown";
}

static esp_err_t logs_page_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_default_creds_redirect(req);
        return ESP_OK;
    }

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)logs_html_start,
                     logs_html_end - logs_html_start - 1);
    return ESP_OK;
}

// Longest possible trailer appended after the packing loop in
// logs_get_handler(), with every %u at 10 digits and all three level names at
// their longest ("verbose"): 153 bytes, plus the NUL vsnprintf writes. Rounded
// up so the two stay decoupled - the reservation only has to be an upper bound.
#define LOGS_TRAILER_MAX 192

// Serves log lines newer than ?since=<seq>, following the same cursor contract
// as /api/client/history: the caller echoes back the "seq" from the previous
// response and gets only what it hasn't seen.
//
// The cursor lives entirely on the client, so reads are non-destructive and any
// number of browser tabs (or curl loops) can tail the log independently - each
// at its own position. A queue that consumed on read would force them to
// compete for lines, and the page would have to police having only one tab open.
static esp_err_t logs_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_setup_required(req);
        return ESP_OK;
    }

    char query[64];
    char since_str[16] = {0};
    // No query at all (ESP_ERR_NOT_FOUND) is the fresh-reader case and falls
    // through to since == 0. A query too long for the buffer is not: the IDF
    // returns ESP_ERR_HTTPD_RESULT_TRUNC without copying anything, so lumping
    // it in with "no query" would replay the whole ring and report lost == 0 -
    // telling a reader that had a position it had missed nothing. Only a
    // cursor we actually read can be trusted to mean that.
    esp_err_t query_err = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (query_err == ESP_ERR_HTTPD_RESULT_TRUNC) {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "Query string too long");
        return ESP_FAIL;
    }
    if (query_err == ESP_OK) {
        httpd_query_key_value(query, "since", since_str, sizeof(since_str));
    }
    uint32_t since = since_str[0] ? (uint32_t)strtoul(since_str, NULL, 10) : 0;

    uint32_t oldest = 0, newest = 0, missed = 0;
    log_buf_get_range(&oldest, &newest, &missed);

    // A cursor ahead of the newest line means the device rebooted under this
    // reader (sequence numbers restart at 1), so start it over from the top.
    bool restarted = since > newest;
    uint32_t next = restarted ? oldest : (since + 1);
    // Lines that aged out of the ring between polls. Reported so the page can
    // show a gap marker instead of silently splicing over the missing ones.
    // since == 0 is a reader starting fresh, which hasn't missed anything -
    // only a reader that had a position and fell behind it has.
    uint32_t lost = 0;
    if (next < oldest) {
        lost = (since == 0) ? 0 : (oldest - next);
        next = oldest;
    }

    // Static, not a local: the 8KB worker stack is shared with the OTA
    // handler's call chain into esp_ota_write() (see ota_buf below).
    static char resp[4096];
    int off = resp_append(resp, sizeof(resp), 0, "{\"lines\":[");

    uint32_t seq = next;
    int emitted = 0;
    for (; seq <= newest; seq++) {
        char level = '?';
        uint32_t ts_ms = 0;
        char tag[LOG_BUF_TAG_MAX];
        char msg[LOG_BUF_MSG_MAX];
        // Copied out before appending, never while holding log_buf's lock:
        // resp_append() logs on truncation, which would re-enter the capture
        // hook and deadlock against a lock this handler was still holding.
        if (!log_buf_get_line(seq, &level, &ts_ms, tag, sizeof(tag), msg, sizeof(msg))) {
            // Overwritten between the range read and here - the loop drops the
            // ring's lock between lines, so a burst can wrap past a batch still
            // being packed. Counted, because skipping it silently would advance
            // the cursor over the gap and splice the two halves together with
            // nothing to show for it. This is counted even for a fresh reader
            // (which starts with lost == 0 by definition): the gap falls inside
            // the lines it is being handed, not before them.
            lost++;
            continue;
        }

        // Stop before the append rather than after: a partially written entry
        // would have to be unwound, and resp_send_json() would reject the whole
        // response anyway. Leaving it for the next poll (via "more") costs one
        // extra round trip and keeps every response complete.
        //
        // The reservation covers the trailer as well as the entry. It has to:
        // packing up to the end of the buffer and only then discovering the
        // trailer doesn't fit produces a 500, and since the client can't
        // advance its cursor past a response it couldn't parse, the next poll
        // rebuilds the same over-packed batch and fails identically. The page
        // stays on "Lost connection" until the ring wraps past that cursor.
        //
        // 64 covers the entry's own fixed JSON (54 bytes at 10-digit seq and
        // timestamp) with room for vsnprintf's NUL; both tag and msg get the
        // 6x allowance because both go through json_append_escaped(), which
        // can expand any byte to \uXXXX. Worst case is 64 + 6 * (31 + 143) =
        // 1108, so no line is ever too large to emit - one that was would wedge
        // the cursor here permanently, which is the state this check prevents.
        size_t needed = 64 + 6 * strlen(tag) + 6 * strlen(msg);
        if ((size_t)off + needed + LOGS_TRAILER_MAX >= sizeof(resp)) {
            break;
        }

        off = resp_append(resp, sizeof(resp), off, "%s{\"n\":%u,\"l\":\"%c\",\"t\":%u,\"g\":\"",
                           emitted == 0 ? "" : ",", (unsigned)seq, level, (unsigned)ts_ms);
        off = json_append_escaped(resp, sizeof(resp), off, tag);
        off = resp_append(resp, sizeof(resp), off, "\",\"m\":\"");
        off = json_append_escaped(resp, sizeof(resp), off, msg);
        off = resp_append(resp, sizeof(resp), off, "\"}");
        emitted++;
    }

    off = resp_append(resp, sizeof(resp), off,
                       "],\"seq\":%u,\"lost\":%u,\"missed\":%u,\"more\":%s,"
                       "\"restarted\":%s,\"level\":\"%s\",\"serial_level\":\"%s\","
                       "\"max_level\":\"%s\"}",
                       (unsigned)(seq - 1), (unsigned)lost, (unsigned)missed,
                       seq <= newest ? "true" : "false",
                       restarted ? "true" : "false",
                       log_level_name(esp_log_level_get("*")),
                       log_level_name(log_buf_get_serial_level()),
                       // What the build kept, not what it will accept: ESP_LOGD
                       // and ESP_LOGV compile to nothing above this, so a
                       // capture level past it records not one extra line. The
                       // page needs it to say so instead of leaving the reader
                       // staring at an unchanged log.
                       log_level_name((esp_log_level_t)CONFIG_LOG_MAXIMUM_LEVEL));

    if (!resp_send_json(req, resp, off, sizeof(resp))) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Sets the capture level - how much reaches the ring the log page reads. The
// serial console's own threshold is separate and stays where "loglevel" put it
// (see log_buf.h), except that it can never exceed this one.
//
// All six names stay accepted here, including "none", even though the page no
// longer offers it: reaching for the API directly is a deliberate act, and
// "loglevel" at the console can raise the level back out of any of them.
static esp_err_t logs_level_post_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_setup_required(req);
        return ESP_OK;
    }

    char buf[64];
    if (recv_body(req, buf, sizeof(buf)) < 0) {
        return ESP_FAIL;
    }

    char level_str[16];
    if (!json_get_string(buf, "level", level_str, sizeof(level_str))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing \"level\"");
        return ESP_FAIL;
    }

    for (size_t i = 0; i < sizeof(log_level_names) / sizeof(log_level_names[0]); i++) {
        if (strcmp(level_str, log_level_names[i].name) == 0) {
            // The announcement has to survive its own change, and which side of
            // the set that takes depends on the direction. A move down to
            // "error" or "none" filters it out if logged after, leaving the
            // ring with no record of why it went quiet; a move up out of
            // "error" or "none" is filtered by the old level if logged before,
            // leaving no record of the raise. Log on the more permissive side
            // either way.
            esp_log_level_t old_level = esp_log_level_get("*");
            esp_log_level_t new_level = log_level_names[i].level;
            if (new_level > old_level) {
                esp_log_level_set("*", new_level);
                ESP_LOGW(TAG, "Log capture level set to %s", log_level_names[i].name);
            } else {
                ESP_LOGW(TAG, "Log capture level set to %s", log_level_names[i].name);
                esp_log_level_set("*", new_level);
            }
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
    }

    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                         "Unknown level (want none|error|warn|info|debug|verbose)");
    return ESP_FAIL;
}

httpd_handle_t web_server_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    // Default 4KB is too tight once the OTA handler's call chain into
    // esp_ota_write()/SPI flash is included on this same worker task's stack.
    config.stack_size = 8192;
    // The default (8) is short of the routes registered below, and
    // httpd_register_uri_handler()'s return value is not checked - overshooting
    // the cap silently drops whichever routes came last, with no error logged.
    // Keep comfortable headroom over the actual count for that reason.
    config.max_uri_handlers = 16;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return NULL;
    }

    const httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_get_handler};
    const httpd_uri_t admin_page_uri = {.uri = "/admin", .method = HTTP_GET, .handler = admin_get_handler};
    const httpd_uri_t status_uri = {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler};
    const httpd_uri_t wifi_uri = {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post_handler};
    const httpd_uri_t admin_uri = {.uri = "/api/admin", .method = HTTP_POST, .handler = admin_post_handler};
    const httpd_uri_t clients_uri = {.uri = "/api/clients", .method = HTTP_GET, .handler = clients_get_handler};
    const httpd_uri_t history_uri = {.uri = "/api/client/history", .method = HTTP_GET, .handler = client_history_get_handler};
    const httpd_uri_t system_uri = {.uri = "/api/system", .method = HTTP_GET, .handler = system_get_handler};
    const httpd_uri_t ota_uri = {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_post_handler};
    const httpd_uri_t logs_page_uri = {.uri = "/logs", .method = HTTP_GET, .handler = logs_page_get_handler};
    const httpd_uri_t logs_uri = {.uri = "/api/logs", .method = HTTP_GET, .handler = logs_get_handler};
    const httpd_uri_t logs_level_uri = {.uri = "/api/logs/level", .method = HTTP_POST, .handler = logs_level_post_handler};

    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &admin_page_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &wifi_uri);
    httpd_register_uri_handler(server, &admin_uri);
    httpd_register_uri_handler(server, &clients_uri);
    httpd_register_uri_handler(server, &history_uri);
    httpd_register_uri_handler(server, &system_uri);
    httpd_register_uri_handler(server, &ota_uri);
    httpd_register_uri_handler(server, &logs_page_uri);
    httpd_register_uri_handler(server, &logs_uri);
    httpd_register_uri_handler(server, &logs_level_uri);

    ESP_LOGI(TAG, "Web server started");
    return server;
}
