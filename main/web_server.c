#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include "web_server.h"
#include "wifi_cfg.h"
#include "auth_cfg.h"
#include "client_track.h"
#include "dhcp_server.h"
#include "sys_monitor.h"
#include "eth_link.h"
#include "reset_log.h"
#include "clock_time.h"
#include "clock_cfg.h"
#include "log_buf.h"
#include "syslog.h"
#include "syslog_cfg.h"
#include "wan.h"
#include "wan_cfg.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web_server";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t admin_html_start[] asm("_binary_admin_html_start");
extern const uint8_t admin_html_end[] asm("_binary_admin_html_end");
extern const uint8_t logs_html_start[] asm("_binary_logs_html_start");
extern const uint8_t logs_html_end[] asm("_binary_logs_html_end");
extern const uint8_t leases_html_start[] asm("_binary_leases_html_start");
extern const uint8_t leases_html_end[] asm("_binary_leases_html_end");
extern const uint8_t resets_html_start[] asm("_binary_resets_html_start");
extern const uint8_t resets_html_end[] asm("_binary_resets_html_end");
extern const uint8_t wan_html_start[] asm("_binary_wan_html_start");
extern const uint8_t wan_html_end[] asm("_binary_wan_html_end");
extern const uint8_t lan_html_start[] asm("_binary_lan_html_start");
extern const uint8_t lan_html_end[] asm("_binary_lan_html_end");

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

// Throttles credential guessing: a rejected *guess* costs a full second,
// capping a brute-force run at ~1 guess/sec instead of as fast as the server
// can answer. The httpd runs a single worker task, so this parks the whole
// server for that second - which is why send_auth_error() spends it only on
// requests that actually presented credentials, never on the header-less
// first request of the Basic-auth handshake. What is left is a caller who
// already chose to sit through a second per attempt, plus the UI's polling
// loops, which stop on 401 rather than free-run at 1Hz (see index.html's
// poll()).
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
// put in the log ring, and nothing else paces it: that caller sends no
// credentials, so AUTH_FAIL_DELAY_MS never applies to them and their requests
// come back as fast as the server can answer. Unmetered, a bare
// `while :; do curl -s http://bridge/api/status; done` writes LOG_BUF_LINES
// (128) - the whole ring - in a fraction of a second, quietly erasing every
// line describing what the device was actually doing, which is exactly the
// history worth having while someone is probing the box.
//
// This interval is therefore the entire limit. At most one line goes out per
// window; the rest are counted and reported on the next one. A probe still
// shows up in the log - it just costs a line a minute however hard it is
// driven, leaving the ring holding hours of everything else.
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

// had_credentials picks the wording: a caller grinding passwords and one that
// never sends a header at all read very differently, and the server now treats
// them differently too (see send_auth_error()), so the log should say which it
// saw. Both share one counter and one window - the suppressed tail below is
// worded generically because a window can hold a mix of the two.
static void log_auth_failure(httpd_req_t *req, bool had_credentials)
{
    const char *why = had_credentials ? "bad credentials" : "no credentials sent";
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
        ESP_LOGW(TAG, "Rejected request for %.*s: %s",
                 AUTH_FAIL_LOG_URI_MAX, req->uri, why);
        return;
    }
    // "since the last of these" rather than "in the last minute": the count is
    // carried until there is a line to print it on, so a burst that stops
    // mid-window is still reported by whatever rejection comes next, however
    // much later that is, instead of being dropped for want of an occasion.
    ESP_LOGW(TAG, "Rejected request for %.*s: %s "
                  "(%u more since the last of these)",
             AUTH_FAIL_LOG_URI_MAX, req->uri, why, (unsigned)also);
}

static void send_auth_error(httpd_req_t *req)
{
    // Credentials that were sent and rejected are a guess, and get the throttle.
    // A request with no Authorization header at all is not one: it is the first
    // half of the Basic-auth handshake, which every browser opens with and which
    // check_admin_auth() has no choice but to fail. Delaying that would put a
    // second in front of every fresh page load, and - the httpd having a single
    // worker task - would let a stranger park the whole server without knowing,
    // or even attempting, a single credential.
    //
    // A malformed or over-long header still counts as present: it was an attempt.
    bool had_credentials = httpd_req_get_hdr_value_len(req, "Authorization") > 0;
    if (had_credentials) {
        vTaskDelay(pdMS_TO_TICKS(AUTH_FAIL_DELAY_MS));
    }
    log_auth_failure(req, had_credentials);

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

// Drops a ":80" suffix so the Origin and Host spellings of the same address
// compare equal - a browser leaves the default port out of Origin, while a
// client may well spell it out in Host, or the other way round.
static void strip_default_port(char *authority)
{
    size_t len = strlen(authority);
    const char *port = ":80";
    size_t port_len = strlen(port);
    if (len > port_len && strcmp(authority + len - port_len, port) == 0) {
        authority[len - port_len] = '\0';
    }
}

// Caps the caller-chosen part of the URI in the lines below, for the reason
// AUTH_FAIL_LOG_URI_MAX gives: req->uri carries the query string, and an
// unbounded one would push the rest of the message past LOG_BUF_MSG_MAX.
#define CSRF_LOG_URI_MAX 48

// Guards the state-changing POSTs against a request this device's own pages did
// not make. Basic Auth alone can't: the credentials are ambient, so the browser
// replays them on any request to this origin, including one a page on some other
// site made. And json_get_string() is a strstr() over the raw body, which parses
// a text/plain body exactly as happily as a JSON one - so a form post or a
// no-cors fetch() from an attacker's page, both CORS *simple* requests that go
// out with no preflight to ask us about, used to arrive here fully authenticated
// and be acted on. One click on the wrong link while signed in was enough to
// change the AP password, take over the admin account, or flash firmware.
//
// Requiring the exact Content-Type is what closes that. Neither
// application/json nor application/octet-stream is a simple content type, so
// sending either cross-origin costs a preflight OPTIONS - and no OPTIONS handler
// is registered here, so the browser blocks the request before it is ever made.
// The Origin check is defence in depth behind it. Both are free for the pages
// themselves, which already send these exact headers.
//
// Answers the caller itself and returns false, rather than pairing with a
// send_*() the way check_admin_auth() does: the rejection comes in two flavours,
// and neither is shared with any other path. No AUTH_FAIL_DELAY_MS either - this
// is only reachable by a caller that authenticated, so there is no guessing to
// throttle, and (see send_setup_required()) the one worker task must not be
// parked on a legitimate request. That also means the log lines here need no
// rate limit: a stranger can't reach them.
static bool csrf_check(httpd_req_t *req, const char *expected_type)
{
    char type[64];
    // Absent, or too long to be one of the two short types we accept.
    bool type_ok = httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) == ESP_OK;
    if (type_ok) {
        // "application/json; charset=utf-8" is the same media type as
        // "application/json" - keep the type, drop the parameters and any
        // padding around them.
        char *semi = strchr(type, ';');
        if (semi != NULL) {
            *semi = '\0';
        }
        char *start = type;
        while (*start == ' ' || *start == '\t') {
            start++;
        }
        size_t type_len = strlen(start);
        while (type_len > 0 && (start[type_len - 1] == ' ' || start[type_len - 1] == '\t')) {
            start[--type_len] = '\0';
        }
        type_ok = strcasecmp(start, expected_type) == 0;
    }
    if (!type_ok) {
        ESP_LOGW(TAG, "Rejected %.*s: Content-Type is not %s",
                 CSRF_LOG_URI_MAX, req->uri, expected_type);
        char resp[128];
        int len = resp_append(resp, sizeof(resp), 0,
                              "{\"ok\":false,\"error\":\"Send this as Content-Type: %s\"}",
                              expected_type);
        httpd_resp_set_status(req, "415 Unsupported Media Type");
        resp_send_json(req, resp, len, sizeof(resp));
        return false;
    }

    char origin[64];
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin));
    if (err == ESP_ERR_NOT_FOUND) {
        // No Origin means no browser behind the request: curl and scripts send
        // none, and no browser omits it on a cross-origin POST. The README
        // documents scripted API access, so this stays allowed.
        return true;
    }

    bool same_origin = false;
    const char *scheme = "http://";
    char host[64];
    // Anything that isn't a plain-http origin can't be this server, which serves
    // http on port 80 only - the literal "null" a sandboxed frame sends included.
    if (err == ESP_OK && strncasecmp(origin, scheme, strlen(scheme)) == 0 &&
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK) {
        char *origin_host = origin + strlen(scheme);
        strip_default_port(origin_host);
        strip_default_port(host);
        same_origin = strcasecmp(origin_host, host) == 0;
    }
    if (!same_origin) {
        ESP_LOGW(TAG, "Rejected %.*s: cross-origin request", CSRF_LOG_URI_MAX, req->uri);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Cross-origin request refused\"}",
                        HTTPD_RESP_USE_STRLEN);
        return false;
    }
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
    if (!csrf_check(req, "application/json")) {
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
    //
    // Tagged before the delay, not after: an untagged software reset is
    // indistinguishable from a panic that got tidied up on the way out, and
    // this one has a person behind it.
    reset_log_note_intent(RESET_INTENT_WIFI_SAVE);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t admin_post_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (!csrf_check(req, "application/json")) {
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
    if (!csrf_check(req, "application/octet-stream")) {
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
    // before tearing everything down for the reboot into the new image, and
    // tag the restart ahead of the delay.
    reset_log_note_intent(RESET_INTENT_OTA);
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

// Renders a time_t as a quoted JSON string, or the literal `null` when the
// device has no time to put there. `out` must be JSON_TIME_MAX bytes.
//
// Null rather than an empty string, and factored out rather than repeated,
// because the rule is easy to get subtly wrong in one of several places: a
// reader must not be able to mistake "this boot never had a clock" for a time
// that failed to render. Same reasoning as ip_or_empty() above, and the same
// shape the reset-history fields (`reached_ready`, `rail_held`) already use.
#define JSON_TIME_MAX (CLOCK_TIME_STR_MAX + 2)
static void json_time_or_null(char *out, size_t outsz, bool have, time_t t)
{
    if (!have) {
        strlcpy(out, "null", outsz);
        return;
    }
    char rendered[CLOCK_TIME_STR_MAX];
    clock_time_format(t, rendered, sizeof(rendered));
    snprintf(out, outsz, "\"%s\"", rendered);
}

// Renders an address for JSON, but as an empty string when it is 0. A lease
// this server never handed out, or a mapping never committed to flash, is an
// absent address rather than 0.0.0.0 - emitting the sentinel would make the
// page recognise it, and one missed check would put "0.0.0.0" in front of a
// reader as if it meant something.
static const char *ip_or_empty(esp_ip4_addr_t ip, char buf[16])
{
    if (ip.addr == 0) {
        return "";
    }
    esp_ip4addr_ntoa(&ip, buf, 16);
    return buf;
}

// The DHCP lease table, live state alongside what is committed to flash for the
// same MAC. Hostnames are not the DHCP server's to keep - it forwards option 12
// on to client_track.c - so they are joined back on here by MAC, and a lease
// whose client isn't currently tracked simply has no name.
static esp_err_t leases_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_setup_required(req);
        return ESP_OK;
    }

    dhcp_lease_info_t leases[DHCP_SERVER_MAX_LEASES];
    int count = dhcp_server_get_leases(leases, DHCP_SERVER_MAX_LEASES);

    client_info_t clients[CLIENT_TRACK_MAX_CLIENTS];
    int client_count = 0;
    client_track_get_snapshot(clients, CLIENT_TRACK_MAX_CLIENTS, &client_count);

    client_track_conflict_t conflicts[CLIENT_TRACK_MAX_CONFLICTS];
    int conflict_count = client_track_get_conflicts(conflicts, CLIENT_TRACK_MAX_CONFLICTS);

    // 231 bytes is a measured worst-case entry with an empty name - four
    // 15-character dotted quads, two 10-digit u32s, and the keys - so 240
    // leaves a little room for a field being added without this being
    // recalculated. On top of that, the 6x expansion of JSON-escaping a
    // control character in a hostname: only a currently-tracked client has a
    // name at all, so that allowance is sized to CLIENT_TRACK_MAX_CLIENTS
    // rather than to the lease count, which is twice as large.
    //
    // The conflict pair is sized the same way rather than being folded into the
    // 240: "conflict" and "conflict_with" together run to about 56 bytes, which
    // would take a worst-case entry to 287 and overrun. But a conflict has two
    // ends and there are only CLIENT_TRACK_MAX_CONFLICTS of them, so at most
    // twice that many entries can ever carry the fields - a couple of hundred
    // bytes, against the 2.5KB of .bss that widening every entry would cost for
    // a case that cannot happen.
    static char resp[DHCP_SERVER_MAX_LEASES * 240 +
                     CLIENT_TRACK_MAX_CLIENTS * 6 * CLIENT_TRACK_NAME_MAX_LEN +
                     CLIENT_TRACK_MAX_CONFLICTS * 2 * 64 + 64];
    int off = resp_append(resp, sizeof(resp), 0,
                           "{\"max\":%d,\"restored\":%d,\"leases\":[",
                           DHCP_SERVER_MAX_LEASES, dhcp_server_get_restored_count());
    for (int i = 0; i < count && off < sizeof(resp); i++) {
        dhcp_lease_info_t *l = &leases[i];

        const char *name = "";
        for (int j = 0; j < client_count; j++) {
            if (memcmp(clients[j].mac, l->mac, 6) == 0) {
                name = clients[j].name;
                break;
            }
        }

        char ip_buf[16], observed_buf[16], saved_buf[16], saved_observed_buf[16];
        off = resp_append(resp, sizeof(resp), off,
                           "%s{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"ip\":\"%s\","
                           "\"observed_ip\":\"%s\",\"expires_in_s\":%u,"
                           "\"boots_since_seen\":%u,\"stored\":%s,"
                           "\"saved_ip\":\"%s\",\"saved_observed_ip\":\"%s\",\"name\":\"",
                           i == 0 ? "" : ",",
                           l->mac[0], l->mac[1], l->mac[2], l->mac[3], l->mac[4], l->mac[5],
                           ip_or_empty(l->ip, ip_buf),
                           ip_or_empty(l->observed_ip, observed_buf),
                           (unsigned)l->expires_in_s, (unsigned)l->boots_since_seen,
                           l->stored ? "true" : "false",
                           ip_or_empty(l->saved_ip, saved_buf),
                           ip_or_empty(l->saved_observed_ip, saved_observed_buf));
        // Same untrusted field as in clients_get_handler(): the hostname is
        // whatever the client called itself in option 12.
        off = json_append_escaped(resp, sizeof(resp), off, name);
        off = resp_append(resp, sizeof(resp), off, "\"");

        // Emitted only for the two ends of a live conflict - see the buffer
        // note above. "blocked" is the device being cut off, "holder" the one
        // keeping the address; conflict_with names the other end either way.
        for (int j = 0; j < conflict_count; j++) {
            if (!conflicts[j].armed) {
                continue;
            }
            bool blocked = memcmp(conflicts[j].mac, l->mac, 6) == 0;
            bool holder = memcmp(conflicts[j].peer_mac, l->mac, 6) == 0;
            if (!blocked && !holder) {
                continue;
            }
            const uint8_t *other = blocked ? conflicts[j].peer_mac : conflicts[j].mac;
            off = resp_append(resp, sizeof(resp), off,
                               ",\"conflict\":\"%s\","
                               "\"conflict_with\":\"%02x:%02x:%02x:%02x:%02x:%02x\"",
                               blocked ? "blocked" : "holder",
                               other[0], other[1], other[2], other[3], other[4], other[5]);
            break;
        }
        off = resp_append(resp, sizeof(resp), off, "}");
    }
    off = resp_append(resp, sizeof(resp), off, "]}");

    if (!resp_send_json(req, resp, off, sizeof(resp))) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Why this device restarted, newest first. Each record straddles two boots on
// purpose - reason and uptime_s describe how the previous one ended, version and
// partition what came up afterwards.
//
// reached_ready is null whenever the RTC counter did not survive, which is the
// normal case after a power cycle; see reset_log.h for why that absence is itself
// the evidence. uptime_s follows it down only when the flash checkpoint has
// nothing for that boot either - where it does, uptime_s and uptime_max_s bound
// the duration from below and above, and uptime_approx says it is a window
// rather than a reading. A client that ignores the window and reads uptime_s
// alone will understate a duration, never overstate one, which is why zero is a
// safe value to send there: it means "under uptime_max_s", not "no time at all".
//
// when/when_epoch are the wall-clock instant the boot OPENED BY this record
// started, and are null together on any record whose boot never had a
// trustworthy clock - see RESET_LOG_F_BOOT_TIME. A client must treat them as
// extra evidence rather than as the record's identity: the ordering and the
// durations are complete on their own and are all that most records will ever
// carry.
static esp_err_t resets_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_setup_required(req);
        return ESP_OK;
    }

    reset_log_entry_t recs[RESET_LOG_MAX_RECORDS];
    int count = reset_log_get(recs, RESET_LOG_MAX_RECORDS);

    // 329 bytes is a measured worst-case entry with an empty version - the
    // longest reason and intent tokens, seq and uptimes at 10 digits, and the
    // keys - so 352 still leaves room, but less than it looks: it was 198 before
    // uptime_approx, 220 before uptime_max_s (which used up the old 240), 246
    // before rail_held added "rail_held":false and its comma, and 265 before the
    // two timestamp fields took 64 between them ("when" at a 19-character
    // rendering plus quotes, "when_epoch" budgeted for a signed 64-bit value
    // rather than the 10 digits it will hold for the next eighty years).
    // Recheck this rather than assume it when adding the next one; the previous
    // allowance had run out and this one is not generous either.
    // The measurement caps reason_code at 3 digits because it is a uint8_t. On top
    // of that the 6x
    // expansion of JSON-escaping a version string: that string was written by a
    // *different* firmware image than the one running, which puts it on the
    // device-supplied side of json_append_escaped()'s rule even though nothing
    // on the network chose it. Static because the httpd worker's 8KB stack is
    // shared with the OTA path, same as the other list endpoints here.
    static char resp[RESET_LOG_MAX_RECORDS * 352 +
                     RESET_LOG_MAX_RECORDS * 6 * RESET_LOG_VERSION_MAX + 96];
    int off = resp_append(resp, sizeof(resp), 0,
                           "{\"max\":%d,\"count\":%d,\"resets\":[",
                           RESET_LOG_MAX_RECORDS, count);
    for (int i = 0; i < count && off < sizeof(resp); i++) {
        reset_log_entry_t *e = &recs[i];
        bool have_prev = (e->flags & RESET_LOG_F_PREV_STATE) != 0;
        // The checkpoint path carries a duration and nothing else, so it fills in
        // uptime_s and leaves reached_ready null - a figure written an hour into a
        // boot is no evidence about how that boot started.
        bool approx = !have_prev && (e->flags & RESET_LOG_F_PREV_UPTIME_MIN) != 0;

        char uptime_str[12] = "null";
        char uptime_max_str[12] = "null";
        const char *ready_str = "null";
        if (have_prev || approx) {
            snprintf(uptime_str, sizeof(uptime_str), "%u", (unsigned)e->prev_uptime_s);
        }
        if (approx) {
            // Only meaningful on the checkpoint path: an exact figure has no
            // upper end to give, and sending one would invite a range around a
            // number that does not need one. Zero back from the ceiling means the
            // schedule has run out - a boot up more than a year - and stays null
            // rather than being sent as a bound of zero.
            uint32_t ceil_s = reset_log_uptime_ceiling(e->prev_uptime_s);
            if (ceil_s != 0) {
                snprintf(uptime_max_str, sizeof(uptime_max_str), "%u", (unsigned)ceil_s);
            }
        }
        if (have_prev) {
            ready_str = (e->flags & RESET_LOG_F_REACHED_READY) ? "true" : "false";
        }

        // Null rather than false when no witness ran, for the same reason
        // reached_ready is: the two mean "the rail stayed up" and "nothing
        // measured it", and collapsing them would report every record written
        // before this existed as a power cut.
        const char *rail_str = "null";
        if (e->flags & RESET_LOG_F_RAIL_KNOWN) {
            rail_str = (e->flags & RESET_LOG_F_RAIL_HELD) ? "true" : "false";
        }

        // Two fields for one fact, and the split is deliberate. "when" is
        // rendered here because the device owns the timezone and a browser
        // cannot parse a POSIX TZ string, so letting the client format would put
        // the web page in a different zone from the console on the same device.
        // "when_epoch" is for arithmetic only - the gap between two boots, which
        // is the same number in every zone - so a client never has to parse the
        // string back to compute one.
        //
        // Null on both when the record has no time, which is every record
        // written before this firmware and every boot that never reached an NTP
        // server. Null rather than an empty string, matching reached_ready and
        // rail_held: the reader must not be able to mistake "no clock" for a
        // time it failed to read.
        bool have_when = (e->flags & RESET_LOG_F_BOOT_TIME) != 0;
        char when_str[JSON_TIME_MAX];
        json_time_or_null(when_str, sizeof(when_str), have_when, (time_t)e->boot_epoch);
        char when_epoch_str[24] = "null";
        if (have_when) {
            snprintf(when_epoch_str, sizeof(when_epoch_str), "%lld", (long long)e->boot_epoch);
        }

        // reason_code carries the raw esp_reset_reason_t beside the token, so a
        // value this build's switch does not enumerate - a newer IDF, or a record
        // written by a later firmware into the same struct version - is still
        // readable by whoever is debugging.
        off = resp_append(resp, sizeof(resp), off,
                           "%s{\"seq\":%u,\"reason\":\"%s\",\"reason_code\":%u,"
                           "\"intent\":\"%s\",\"uptime_s\":%s,\"uptime_approx\":%s,"
                           "\"uptime_max_s\":%s,\"reached_ready\":%s,"
                           "\"rail_held\":%s,"
                           "\"when\":%s,\"when_epoch\":%s,"
                           "\"ota_pending\":%s,\"rollback\":%s,"
                           "\"partition\":\"%s\",\"version\":\"",
                           i == 0 ? "" : ",",
                           (unsigned)e->boot_seq,
                           reset_log_reason_name(e->reason), (unsigned)e->reason,
                           reset_log_intent_name(e->intent),
                           uptime_str, approx ? "true" : "false", uptime_max_str, ready_str,
                           rail_str,
                           when_str, when_epoch_str,
                           (e->flags & RESET_LOG_F_OTA_PENDING) ? "true" : "false",
                           (e->flags & RESET_LOG_F_ROLLBACK) ? "true" : "false",
                           // A partition-table label, a build artefact of this
                           // image rather than of the one that wrote the record.
                           e->part);
        off = json_append_escaped(resp, sizeof(resp), off, e->version);
        off = resp_append(resp, sizeof(resp), off, "\"}");
    }
    off = resp_append(resp, sizeof(resp), off, "]}");

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

static esp_err_t kick_post_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_setup_required(req);
        return ESP_OK;
    }
    if (!csrf_check(req, "application/json")) {
        return ESP_OK;
    }

    char buf[64];
    if (recv_body(req, buf, sizeof(buf)) < 0) {
        return ESP_FAIL;
    }

    char mac_str[24] = {0};
    uint8_t mac[6];
    if (!json_get_string(buf, "mac", mac_str, sizeof(mac_str)) || !parse_mac(mac_str, mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid \"mac\"");
        return ESP_FAIL;
    }

    char resp[96];
    int len;
    if (client_track_kick_station(mac)) {
        len = resp_append(resp, sizeof(resp), 0, "{\"ok\":true}");
    } else {
        len = resp_append(resp, sizeof(resp), 0,
                           "{\"ok\":false,\"error\":\"Not a connected WiFi client\"}");
    }
    if (!resp_send_json(req, resp, len, sizeof(resp))) {
        return ESP_FAIL;
    }
    return ESP_OK;
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

    client_track_fwd_drops_t fwd;
    client_track_get_forward_drops(&fwd);

    uint32_t wifi_tx_total = 0, wifi_tx_failed = 0;
    client_track_get_wifi_tx(&wifi_tx_total, &wifi_tx_failed);

    eth_link_status_t eth;
    eth_link_get_status(&eth);

    const esp_app_desc_t *app_desc = esp_app_get_description();

    // Why this boot happened. The two unknowable values go out as JSON null
    // rather than as a zero with a companion boolean - same shape clients_get_handler
    // already uses for rssi, and for the same reason: 0 and false would be
    // claims, and after a power cycle the honest answer is that the device
    // cannot know. See reset_log.h.
    // Zeroed rather than left to the have_boot guards below: every read of it is
    // guarded, but a future field added to the format string should not have to
    // rediscover that.
    reset_log_entry_t boot = { 0 };
    bool have_boot = reset_log_get_current(&boot);
    char boot_uptime_str[12] = "null";
    const char *boot_ready_str = "null";
    // Recovered from the flash checkpoint rather than read off the RTC counter,
    // which makes the number the low end of a window and not a reading. Sent as
    // its own boolean rather than folded into the value, so a client that does
    // not know about it still gets a duration it can render - just an
    // understated one.
    char boot_uptime_max_str[12] = "null";
    bool boot_uptime_approx = have_boot && !(boot.flags & RESET_LOG_F_PREV_STATE) &&
                              (boot.flags & RESET_LOG_F_PREV_UPTIME_MIN) != 0;
    if (have_boot && (boot.flags & RESET_LOG_F_PREV_STATE)) {
        snprintf(boot_uptime_str, sizeof(boot_uptime_str), "%u", (unsigned)boot.prev_uptime_s);
        boot_ready_str = (boot.flags & RESET_LOG_F_REACHED_READY) ? "true" : "false";
    } else if (boot_uptime_approx) {
        snprintf(boot_uptime_str, sizeof(boot_uptime_str), "%u", (unsigned)boot.prev_uptime_s);
        // Null rather than 0 once the schedule has run out; see the same case in
        // resets_get_handler().
        uint32_t boot_ceil_s = reset_log_uptime_ceiling(boot.prev_uptime_s);
        if (boot_ceil_s != 0) {
            snprintf(boot_uptime_max_str, sizeof(boot_uptime_max_str), "%u",
                     (unsigned)boot_ceil_s);
        }
    }

    // What the 3.3V rail did across the reset that started this boot; null when
    // nothing measured it. See the same field in resets_get_handler().
    const char *boot_rail_str = "null";
    if (have_boot && (boot.flags & RESET_LOG_F_RAIL_KNOWN)) {
        boot_rail_str = (boot.flags & RESET_LOG_F_RAIL_HELD) ? "true" : "false";
    }

    uint32_t q_up = 0, q_down = 0;
    client_track_get_quarantine_drops(&q_up, &q_down);

    // This boot's start time, and the state of the clock that produced it. Note
    // boot_when comes off the RECORD rather than being recomputed from the
    // uptime: they agree to within a second, and using the record means the
    // dashboard cannot disagree with the /resets row for the same boot.
    char boot_when_str[JSON_TIME_MAX];
    json_time_or_null(boot_when_str, sizeof(boot_when_str),
                      have_boot && (boot.flags & RESET_LOG_F_BOOT_TIME),
                      (time_t)boot.boot_epoch);
    // The rendered string only, unlike /api/resets, which sends an epoch beside
    // it. That endpoint needs one because the page does arithmetic on it - the
    // dark gap between two boots is a subtraction - and this one has no
    // arithmetic to do. Every consumer of these fields either displays them or
    // tests them for null, and the string answers both.
    char now_str[JSON_TIME_MAX];
    char now_zone[16] = "";
    time_t clock_now = 0;
    bool have_now = clock_time_now(&clock_now);
    json_time_or_null(now_str, sizeof(now_str), have_now, clock_now);
    if (have_now) {
        strlcpy(now_zone, clock_time_zone_abbrev(clock_now), sizeof(now_zone));
    }

    // Counted, not estimated. This buffer has twice been set to a figure that
    // looked comfortable and was not: 768 when the real worst case was 930, then
    // 1024 when boot_rail_held took it to 954 and left no room for the clock
    // fields at all. Both figures were estimates, and the second was measured
    // against a short "1.3.0" when esp_app_desc_t.version holds 32 characters.
    //
    // The count, for the format string immediately below: 667 fixed characters,
    // 22 %u at ten digits, one %llu at twenty, and 18 %s worst cases summing to
    // 182 - the longest reason ("deep-sleep") and intent ("factory-reset")
    // tokens, three ten-digit uptimes, two quoted 19-character times, a
    // 15-character zone abbreviation and a 31-character version. That is 1089.
    // Recount when a field is added; adding one is what invalidated the last two
    // figures.
    //
    // 1536 rather than 1280, which would fit. Overflow is caught rather than
    // silent - resp_send_json() sends a 500, and smoke.sh runs this endpoint
    // through jq -e - but a 500 on the endpoint the dashboard polls is a poor
    // way to find out, and the margin is what has been wrong here twice.
    //
    // Static, like the list endpoints above, rather than another 1.5KB on the
    // httpd worker's 8KB stack that the OTA path shares. esp_http_server
    // services every socket from one task, so there is no second caller to race.
    static char resp[1536];
    int len = resp_append(resp, sizeof(resp), 0,
                          "{\"uptime_s\":%llu,\"free_heap\":%u,\"min_free_heap\":%u,"
                          "\"cpu_pct\":[%u,%u],\"cpu_freq_mhz\":%u,\"net_rx_bps\":%u,\"net_tx_bps\":%u,"
                          "\"net_rx_pps\":%u,\"net_tx_pps\":%u,"
                          "\"traffic_drops\":%u,"
                          "\"fwd_drop_eth_in\":%u,\"fwd_drop_wifi_in\":%u,"
                          "\"fwd_drop_eth_out\":%u,\"fwd_drop_wifi_out\":%u,"
                          "\"wifi_tx_total\":%u,\"wifi_tx_failed\":%u,"
                          "\"ip_conflicts\":%u,\"ip_conflict_drops\":%u,"
                          "\"ip_conflict_enforced\":%s,"
                          "\"eth_link\":%s,\"eth_speed_mbit\":%u,\"eth_duplex\":\"%s\","
                          "\"eth_autoneg\":%s,\"eth_flaps\":%u,\"eth_change_s\":%u,"
                          "\"boot_seq\":%u,\"boot_reason\":\"%s\",\"boot_intent\":\"%s\","
                          "\"boot_prev_uptime_s\":%s,\"boot_prev_uptime_approx\":%s,"
                          "\"boot_prev_uptime_max_s\":%s,\"boot_prev_ready\":%s,"
                          "\"boot_rail_held\":%s,\"boot_rollback\":%s,"
                          "\"boot_when\":%s,"
                          "\"now\":%s,\"now_zone\":\"%s\","
                          "\"clock_source\":\"%s\",\"clock_stale\":%s,"
                          "\"version\":\"%s\"}",
                          (unsigned long long)(esp_timer_get_time() / 1000000ULL),
                          (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
                          (unsigned)cpu_pct[0], (unsigned)cpu_pct[1], (unsigned)sys_monitor_get_cpu_freq_mhz(),
                          (unsigned)rx_total, (unsigned)tx_total, (unsigned)rx_pps, (unsigned)tx_pps,
                          (unsigned)client_track_get_traffic_drops(),
                          (unsigned)fwd.eth_in, (unsigned)fwd.wifi_in,
                          (unsigned)fwd.eth_out, (unsigned)fwd.wifi_out,
                          (unsigned)wifi_tx_total, (unsigned)wifi_tx_failed,
                          // A count of live conflicts, not a since-boot total:
                          // this one goes back to zero when the fault is fixed.
                          (unsigned)client_track_get_armed_conflicts(),
                          (unsigned)(q_up + q_down),
                          client_track_quarantine_enforcing() ? "true" : "false",
                          // Fixed internal literals, so no json_append_escaped()
                          // needed - that helper is for device-supplied strings.
                          eth.up ? "true" : "false", (unsigned)eth.speed_mbit,
                          eth.up ? (eth.full_duplex ? "full" : "half") : "",
                          eth.autoneg ? "true" : "false",
                          (unsigned)eth.flaps, (unsigned)eth.since_change_s,
                          // Also fixed internal literals - reset_log_reason_name()
                          // and its intent counterpart return from a closed set,
                          // never a device-supplied string.
                          have_boot ? (unsigned)boot.boot_seq : 0u,
                          have_boot ? reset_log_reason_name(boot.reason) : "unknown",
                          have_boot ? reset_log_intent_name(boot.intent) : "unknown",
                          boot_uptime_str, boot_uptime_approx ? "true" : "false",
                          boot_uptime_max_str, boot_ready_str, boot_rail_str,
                          (have_boot && (boot.flags & RESET_LOG_F_ROLLBACK)) ? "true" : "false",
                          boot_when_str,
                          now_str, now_zone,
                          clock_time_source(),
                          clock_time_stale() ? "true" : "false",
                          app_desc->version);
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

static esp_err_t leases_page_get_handler(httpd_req_t *req)
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
    httpd_resp_send(req, (const char *)leases_html_start,
                     leases_html_end - leases_html_start - 1);
    return ESP_OK;
}

static esp_err_t resets_page_get_handler(httpd_req_t *req)
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
    httpd_resp_send(req, (const char *)resets_html_start,
                     resets_html_end - resets_html_start - 1);
    return ESP_OK;
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

static esp_err_t wan_page_get_handler(httpd_req_t *req)
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
    httpd_resp_send(req, (const char *)wan_html_start,
                     wan_html_end - wan_html_start - 1);
    return ESP_OK;
}

static esp_err_t lan_page_get_handler(httpd_req_t *req)
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
    httpd_resp_send(req, (const char *)lan_html_start,
                     lan_html_end - lan_html_start - 1);
    return ESP_OK;
}

// Longest possible trailer appended after the packing loop in
// logs_get_handler(), with every %u at 10 digits and all three level names at
// their longest ("verbose"): 153 bytes for the log fields, plus 133 for the six
// syslog_* ones (syslog_errno is signed, so eleven characters), plus the NUL
// vsnprintf writes. Rounded up past the 287-byte measurement so the two stay
// decoupled - the reservation only has to be an upper bound.
#define LOGS_TRAILER_MAX 320

// Longest gap entry the packing loop can emit: ",{\"gap\":4294967295}" is 20
// bytes, plus the NUL. Rounded up for the same reason as the trailer.
#define LOGS_GAP_MAX 32

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

    // Read here, before the packing loop, so the handler never calls into
    // another module while it is iterating the ring - the same discipline the
    // loop below follows about not appending under log_buf's lock.
    syslog_status_t syslog_st;
    syslog_get_status(&syslog_st);

    // A cursor ahead of the newest line means the device rebooted under this
    // reader (sequence numbers restart at 1), so start it over from the top.
    bool restarted = since > newest;
    uint32_t next = restarted ? oldest : (since + 1);
    // Lines that aged out of the ring between polls, before the first line of
    // this batch. Reported so the page can show a gap marker instead of
    // silently splicing over the missing ones. Lines that age out *during* the
    // packing loop below are not counted here - they belong inside the batch,
    // not in front of it, and are emitted in place (see "gap").
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
    // Lines dropped since the last entry was emitted, waiting to be written out
    // as a gap entry in front of whichever line follows them.
    uint32_t gap = 0;
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
            // nothing to show for it. Held until the next line is emitted
            // rather than added to "lost": the hole is inside the batch, and a
            // count reported alongside it could only be drawn in front of the
            // whole batch, pointing at the wrong place. A burst can wrap the
            // ring more than once while one response is packed, so there may be
            // several of these runs, each with lines emitted between them.
            gap++;
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
        //
        // Two gap entries are reserved on every pass, whether or not one is
        // pending: the entry may be preceded by a gap that is pending now, and
        // the loop may then drop more lines and have to write a second one on
        // the way out. Both are unconditional because a gap can never be
        // deferred to the next poll - the lines it stands for are below the
        // cursor this response reports, so nothing later will see them.
        size_t needed = 64 + 6 * strlen(tag) + 6 * strlen(msg);
        if ((size_t)off + needed + 2 * LOGS_GAP_MAX + LOGS_TRAILER_MAX >= sizeof(resp)) {
            break;
        }

        if (gap > 0) {
            off = resp_append(resp, sizeof(resp), off, "%s{\"gap\":%u}",
                               emitted == 0 ? "" : ",", (unsigned)gap);
            emitted++;
            gap = 0;
        }

        off = resp_append(resp, sizeof(resp), off, "%s{\"n\":%u,\"l\":\"%c\",\"t\":%u,\"g\":\"",
                           emitted == 0 ? "" : ",", (unsigned)seq, level, (unsigned)ts_ms);
        off = json_append_escaped(resp, sizeof(resp), off, tag);
        off = resp_append(resp, sizeof(resp), off, "\",\"m\":\"");
        off = json_append_escaped(resp, sizeof(resp), off, msg);
        off = resp_append(resp, sizeof(resp), off, "\"}");
        emitted++;
    }

    // Dropped lines with nothing after them: the loop ran out of range, or out
    // of buffer at a line this response won't carry. Either way they are below
    // the cursor this response reports, so the marker goes at the end of the
    // batch rather than being left for a poll that will never see them. The
    // reservation above guarantees the room.
    if (gap > 0) {
        off = resp_append(resp, sizeof(resp), off, "%s{\"gap\":%u}",
                           emitted == 0 ? "" : ",", (unsigned)gap);
    }

    off = resp_append(resp, sizeof(resp), off,
                       "],\"seq\":%u,\"lost\":%u,\"missed\":%u,\"more\":%s,"
                       "\"restarted\":%s,\"level\":\"%s\",\"serial_level\":\"%s\","
                       "\"max_level\":\"%s\"",
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

    // The syslog sender's state rides along on this poll rather than getting a
    // route of its own: the log page already asks for this once a second, so the
    // status line below the log gets live shipping state for no extra request.
    // It is also why these are not on /api/system, whose 1KB buffer is already
    // within 70 bytes of its measured worst case.
    off = resp_append(resp, sizeof(resp), off,
                       ",\"syslog_enabled\":%s,\"syslog_sent\":%u,\"syslog_aged\":%u,"
                       "\"syslog_failed\":%u,\"syslog_errno\":%d,\"syslog_backlog\":%u",
                       syslog_st.enabled ? "true" : "false",
                       (unsigned)syslog_st.sent, (unsigned)syslog_st.aged_out,
                       (unsigned)syslog_st.send_fail, syslog_st.last_errno,
                       (unsigned)syslog_st.backlog);
    off = resp_append(resp, sizeof(resp), off, "}");

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
    if (!csrf_check(req, "application/json")) {
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

// Configuration and live state in one object, so the log page needs a single
// fetch to render the syslog panel. "subnet" is included so the form can say
// what "on this bridge's network" actually means before anything is submitted,
// rather than only in the rejection that comes back if it is wrong.
static esp_err_t syslog_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_setup_required(req);
        return ESP_OK;
    }

    syslog_cfg_t cfg;
    syslog_cfg_get(&cfg);

    syslog_status_t st;
    syslog_get_status(&st);

    uint32_t subnet_ip = 0, subnet_mask = 0;
    syslog_get_subnet(&subnet_ip, &subnet_mask);

    char server[16], subnet[32];
    esp_ip4_addr_t server_addr = { .addr = cfg.server_ip };
    esp_ip4addr_ntoa(&server_addr, server, sizeof(server));

    if (subnet_mask == 0) {
        strlcpy(subnet, "unknown", sizeof(subnet));
    } else {
        esp_ip4_addr_t net = { .addr = subnet_ip & subnet_mask };
        char net_str[16];
        esp_ip4addr_ntoa(&net, net_str, sizeof(net_str));
        // Prefix length from the mask's population count. The mask is
        // contiguous by construction, so counting bits is the whole conversion.
        int bits = 0;
        for (uint32_t m = lwip_ntohl(subnet_mask); m & 0x80000000u; m <<= 1) {
            bits++;
        }
        snprintf(subnet, sizeof(subnet), "%s/%d", net_str, bits);
    }

    // 512 against a measured worst case of ~420: the hostname is user-supplied
    // and goes through json_append_escaped(), which can expand any byte to
    // \uXXXX, so 32 characters can reach 192; the rest is ~200 of fixed JSON
    // with every %u at ten digits, plus two dotted-quad strings.
    char resp[512];
    int off = resp_append(resp, sizeof(resp), 0,
                          "{\"enabled\":%s,\"server\":\"%s\",\"port\":%u,\"facility\":%u,"
                          "\"min_severity\":%u,\"subnet\":\"%s\",\"hostname\":\"",
                          cfg.enabled ? "true" : "false",
                          cfg.server_ip == 0 ? "" : server,
                          (unsigned)cfg.port, (unsigned)cfg.facility,
                          (unsigned)cfg.min_severity, subnet);
    off = json_append_escaped(resp, sizeof(resp), off, cfg.hostname);
    off = resp_append(resp, sizeof(resp), off,
                      "\",\"bound\":%s,\"sent\":%u,\"filtered\":%u,\"aged_out\":%u,"
                      "\"send_fail\":%u,\"errno\":%d,\"backlog\":%u}",
                      st.bound ? "true" : "false",
                      (unsigned)st.sent, (unsigned)st.filtered, (unsigned)st.aged_out,
                      (unsigned)st.send_fail, st.last_errno, (unsigned)st.backlog);

    if (!resp_send_json(req, resp, off, sizeof(resp))) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t syslog_post_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_setup_required(req);
        return ESP_OK;
    }
    if (!csrf_check(req, "application/json")) {
        return ESP_OK;
    }

    char buf[256];
    if (recv_body(req, buf, sizeof(buf)) < 0) {
        return ESP_FAIL;
    }

    // Starts from what is saved, so any field the caller leaves out keeps its
    // current value - the same contract wifi_post_handler() follows.
    syslog_cfg_t cfg;
    syslog_cfg_get(&cfg);

    long val = 0;
    // Sent as 1/0 rather than true/false: json_get_int() reads numbers, and
    // teaching that deliberately-minimal helper about JSON booleans for one
    // field is not the trade to make.
    if (json_get_int(buf, "enabled", &val)) {
        cfg.enabled = (val != 0);
    }
    if (json_get_int(buf, "port", &val)) {
        if (val < 1 || val > 65535) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                 "Port must be 1-65535 (514 is the syslog default)");
            return ESP_FAIL;
        }
        cfg.port = (uint16_t)val;
    }
    if (json_get_int(buf, "facility", &val)) {
        if (val < 0 || val > 23) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                 "Facility must be 0-23 (23 is local7)");
            return ESP_FAIL;
        }
        cfg.facility = (uint8_t)val;
    }
    if (json_get_int(buf, "min_severity", &val)) {
        if (val < 0 || val > 7) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Minimum severity must be 0-7");
            return ESP_FAIL;
        }
        cfg.min_severity = (uint8_t)val;
    }

    char server[16] = {0};
    if (json_get_string(buf, "server", server, sizeof(server))) {
        if (server[0] == '\0') {
            cfg.server_ip = 0;
        } else {
            esp_ip4_addr_t addr;
            addr.addr = esp_ip4addr_aton(server);
            if (addr.addr == 0) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "That is not an IPv4 address");
                return ESP_FAIL;
            }
            cfg.server_ip = addr.addr;
        }
    }

    char hostname[SYSLOG_CFG_HOSTNAME_MAX_LEN] = {0};
    if (json_get_string(buf, "hostname", hostname, sizeof(hostname))) {
        // An explicitly blank name means "go back to the MAC-derived default",
        // which is what the form's empty field with its placeholder shows.
        if (hostname[0] == '\0') {
            syslog_cfg_default_hostname(cfg.hostname, sizeof(cfg.hostname));
        } else {
            strlcpy(cfg.hostname, hostname, sizeof(cfg.hostname));
        }
    }

    uint32_t subnet_ip = 0, subnet_mask = 0;
    syslog_get_subnet(&subnet_ip, &subnet_mask);

    const char *err_msg = NULL;
    if (!syslog_cfg_validate(&cfg, subnet_ip, subnet_mask, &err_msg)) {
        // The static reason verbatim: the whole point of the on-subnet message
        // is that the person who typed the address reads it.
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_msg);
        return ESP_FAIL;
    }

    if (syslog_cfg_save(&cfg) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save syslog config");
        return ESP_FAIL;
    }
    syslog_config_changed();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// The timezone setting, the live clock, and where that clock came from - one
// object, so the admin page renders its Clock section in a single fetch.
//
// The zone list ships with the response rather than being hard-coded in the
// page, for the reason reset_log_reason_name() exists: the console offers the
// same menu, and two copies of a list like this drift. What a caller does with
// "zones" is offer them; "tz" is the only field that decides anything.
static esp_err_t time_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_setup_required(req);
        return ESP_OK;
    }

    clock_cfg_t cfg;
    clock_cfg_get(&cfg);

    char now_str[JSON_TIME_MAX];
    char abbrev[16] = "";
    time_t now = 0;
    bool have_now = clock_time_now(&now);
    json_time_or_null(now_str, sizeof(now_str), have_now, now);
    if (have_now) {
        strlcpy(abbrev, clock_time_zone_abbrev(now), sizeof(abbrev));
    }

    // 0 is the "never synced this power-cycle" sentinel rather than a time, so
    // it is the gate here - see clock_time_last_sync().
    char sync_str[JSON_TIME_MAX];
    time_t last = clock_time_last_sync();
    json_time_or_null(sync_str, sizeof(sync_str), last != 0, last);

    // 2048 against a measured worst case of 1828. The zone table is the bulk of
    // it - 1454 bytes for the 28 entries as JSON, counted rather than estimated,
    // because a first guess of "~40 bytes each" was low by a third and would
    // have sized this buffer under the response it has to hold. It is a
    // compiled-in literal, so it only grows when somebody adds a zone to
    // k_zones[] in clock_cfg.c: check this figure when they do. The remaining
    // 374 is the fixed keys, two 19-character times, the zone abbreviation, and
    // the stored tz, which is user-supplied and goes through
    // json_append_escaped() - that can expand any byte to \uXXXX, so 39
    // characters can reach 234 on their own.
    static char resp[2048];
    int off = resp_append(resp, sizeof(resp), 0, "{\"tz\":\"");
    off = json_append_escaped(resp, sizeof(resp), off, cfg.tz);
    off = resp_append(resp, sizeof(resp), off,
                      "\",\"now\":%s,\"zone\":\"%s\",\"source\":\"%s\","
                      "\"stale\":%s,\"last_sync\":%s,\"zones\":[",
                      now_str, abbrev, clock_time_source(),
                      clock_time_stale() ? "true" : "false", sync_str);

    const char *label, *tz;
    for (int i = 0; clock_cfg_zone(i, &label, &tz); i++) {
        // Both are compiled-in literals from clock_cfg.c, so no escaping is
        // needed - that helper is for device-supplied strings.
        off = resp_append(resp, sizeof(resp), off, "%s{\"label\":\"%s\",\"tz\":\"%s\"}",
                          i == 0 ? "" : ",", label, tz);
    }
    off = resp_append(resp, sizeof(resp), off, "]}");

    if (!resp_send_json(req, resp, off, sizeof(resp))) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Takes effect immediately and deliberately does not reboot, unlike the WiFi
// and WAN saves: a timezone changes how times are rendered and nothing about
// how the device is on the network, so there is nothing to tear down. The next
// poll of any page shows the new zone.
static esp_err_t time_post_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_setup_required(req);
        return ESP_OK;
    }
    if (!csrf_check(req, "application/json")) {
        return ESP_FAIL;
    }

    char buf[128];
    if (recv_body(req, buf, sizeof(buf)) < 0) {
        return ESP_FAIL;
    }

    // Starts from what is saved, so a caller that omits the field keeps the
    // current value - the same contract wifi_post_handler() follows.
    clock_cfg_t cfg;
    clock_cfg_get(&cfg);
    char want[CLOCK_CFG_TZ_MAX_LEN];
    if (!json_get_string(buf, "tz", want, sizeof(want))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing \"tz\"");
        return ESP_FAIL;
    }
    // An IANA label is accepted here as well as a POSIX string, so this endpoint
    // takes the same words the console's "time" command does. Without it a
    // caller posting "America/Chicago" would get no error and a clock in UTC -
    // newlib parses what it recognises and silently treats the rest as UTC, so
    // the mistake surfaces hours later as a wrong timestamp rather than as a
    // rejected request. The web form itself always posts the POSIX string.
    const char *resolved = clock_cfg_zone_from_label(want);
    strlcpy(cfg.tz, resolved ? resolved : want, sizeof(cfg.tz));

    const char *err_msg = NULL;
    if (!clock_cfg_validate(&cfg, &err_msg)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_msg);
        return ESP_FAIL;
    }
    if (clock_cfg_save(&cfg) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save the timezone");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t wan_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_setup_required(req);
        return ESP_OK;
    }

    wan_cfg_t cfg;
    wan_cfg_get(&cfg);

    wan_status_t st;
    wan_get_status(&st);

    char ports[WAN_CFG_PORTS_STR_MAX];
    wan_cfg_format_ports(&cfg, ports, sizeof(ports));

    char ip[16], mask[16], gw[16], dns[16];
    esp_ip4addr_ntoa(&st.ip, ip, sizeof(ip));
    esp_ip4addr_ntoa(&st.netmask, mask, sizeof(mask));
    esp_ip4addr_ntoa(&st.gw, gw, sizeof(gw));
    esp_ip4addr_ntoa(&st.dns, dns, sizeof(dns));

    // 768 against a measured worst case of ~625: the SSID is user-supplied and
    // goes through json_append_escaped(), which can expand any byte to \uXXXX,
    // so 32 characters can reach 192; the port list is at most
    // WAN_CFG_PORTS_STR_MAX (121) now that rules carry a "/udp" suffix and there
    // can be twelve of them; the rest is ~250 of fixed JSON with every %u at ten
    // digits, plus four dotted quads.
    //
    // The password is never sent, matching wifi_get_handler().
    char resp[768];
    int off = resp_append(resp, sizeof(resp), 0, "{\"enabled\":%s,\"ssid\":\"",
                          cfg.enabled ? "true" : "false");
    off = json_append_escaped(resp, sizeof(resp), off, cfg.ssid);
    off = resp_append(resp, sizeof(resp), off,
                      "\",\"ports\":\"%s\",\"state\":\"%s\",\"lan\":\"192.168.5.0/24\","
                      "\"ip\":\"%s\",\"netmask\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\","
                      "\"channel\":%u,\"ap_channel_configured\":%u,\"rssi\":%d,"
                      "\"napt\":%s,\"connects\":%u,\"disconnects\":%u,\"last_reason\":%u,"
                      "\"retry_in_s\":%u,\"tx_allowed\":%u,\"tx_blocked\":%u,"
                      "\"rx_allowed\":%u,\"rx_blocked\":%u}",
                      ports, wan_state_name(st.state),
                      st.ip.addr == 0 ? "" : ip,
                      st.ip.addr == 0 ? "" : mask,
                      st.ip.addr == 0 ? "" : gw,
                      st.dns.addr == 0 ? "" : dns,
                      (unsigned)st.ap_channel, (unsigned)st.ap_channel_configured,
                      (int)st.rssi, st.napt_on ? "true" : "false",
                      (unsigned)st.connects, (unsigned)st.disconnects,
                      (unsigned)st.last_reason, (unsigned)st.retry_in_s,
                      (unsigned)st.tx_allowed, (unsigned)st.tx_blocked,
                      (unsigned)st.rx_allowed, (unsigned)st.rx_blocked);

    if (!resp_send_json(req, resp, off, sizeof(resp))) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t wan_post_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_setup_required(req);
        return ESP_OK;
    }
    if (!csrf_check(req, "application/json")) {
        return ESP_OK;
    }

    // 512 rather than the 256 the syslog handler uses: a full request here is a
    // 32-byte SSID, a 63-byte password and a port list that can reach 121
    // characters, which does not fit in 256 once the JSON around it is counted.
    // recv_body() reports a body that overruns rather than truncating it, so
    // undersizing this would show up as a save that fails, not one that half
    // applies - but a port list nobody can save is still a bug.
    char buf[512];
    if (recv_body(req, buf, sizeof(buf)) < 0) {
        return ESP_FAIL;
    }

    // Starts from what is saved, so any field the caller leaves out keeps its
    // current value - the same contract syslog_post_handler() follows.
    wan_cfg_t cfg;
    wan_cfg_get(&cfg);
    const wan_cfg_t before = cfg;

    long val = 0;
    // 1/0 rather than true/false, for the reason syslog_post_handler() records.
    if (json_get_int(buf, "enabled", &val)) {
        cfg.enabled = (val != 0);
    }

    char ssid[WAN_CFG_SSID_MAX_LEN] = {0};
    if (json_get_string(buf, "ssid", ssid, sizeof(ssid))) {
        strlcpy(cfg.ssid, ssid, sizeof(cfg.ssid));
    }

    // An absent or blank password keeps the stored one, so the form can be
    // re-submitted without the upstream network's password being round-tripped
    // through the browser - the same trick wifi_post_handler() uses.
    char password[WAN_CFG_PASSWORD_MAX_LEN] = {0};
    if (json_get_string(buf, "password", password, sizeof(password)) && password[0] != '\0') {
        strlcpy(cfg.password, password, sizeof(cfg.password));
    }

    char ports[WAN_CFG_PORTS_STR_MAX] = {0};
    if (json_get_string(buf, "ports", ports, sizeof(ports))) {
        const char *err_msg = NULL;
        if (!wan_cfg_parse_ports(ports, cfg.ports, &cfg.nports, &err_msg)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_msg);
            return ESP_FAIL;
        }
    }

    const char *err_msg = NULL;
    if (!wan_cfg_validate(&cfg, &err_msg)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_msg);
        return ESP_FAIL;
    }

    // Which half of the configuration moved decides whether this costs a reboot.
    // The allowlist is read from a published snapshot on every packet, so it can
    // be swapped live; the radio's mode and credentials cannot.
    bool radio_changed = (cfg.enabled != before.enabled) ||
                         strcmp(cfg.ssid, before.ssid) != 0 ||
                         strcmp(cfg.password, before.password) != 0;

    if (wan_cfg_save(&cfg) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save uplink config");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, radio_changed ? "{\"ok\":true,\"reboot\":true}"
                                       : "{\"ok\":true,\"reboot\":false}",
                    HTTPD_RESP_USE_STRLEN);

    if (!radio_changed) {
        wan_ports_changed();
        return ESP_OK;
    }

    // Reboot rather than reconfiguring in place, for the reason
    // wifi_post_handler() records: the AP netif is a bridge port wired into
    // esp_netif_br_glue, and going AP <-> APSTA hot-applies a mode change to it.
    // It costs nothing that was not already lost either way - promoting the
    // radio to APSTA forces it onto the upstream's channel, which drops every
    // associated station regardless.
    reset_log_note_intent(RESET_INTENT_WAN_SAVE);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

// Chunk buffer for the download below. 1024 holds at least five lines at the
// ~193-byte worst case (6 + 2 + 8 + 2 + 1 + 2 + 24 + 2 + 143 + newline), and is
// static for the same reason logs_get_handler()'s response buffer is: the 8KB
// worker stack is shared with the OTA handler's call chain.
#define LOGS_DL_CHUNK 1024

// Flushed whenever this much space is no longer free, so a partial line is never
// sent. It has to be a true upper bound on one line rather than a hope:
// resp_append() logs on truncation, and logging from inside this loop would
// re-enter the capture hook while the ring is being read.
#define LOGS_DL_LINE_MAX 200

// %6u sequence, %8u milliseconds, level letter, %-24s tag, message. Fixed
// columns so the file is greppable and sorts by hand.
//
// 24 rather than LOG_BUF_TAG_MAX: the tag that motivated the 32-byte field
// ("esp_eth.netif.netif_glue", 24 characters) is also the longest that exists,
// so padding to 32 would push every message eight columns right for one tag's
// sake. A longer tag simply widens its own row.
#define LOGS_DL_LINE_FMT "%6u  %8u  %c  %-24s  %s\n"

static int dl_flush(httpd_req_t *req, char *chunk, int off, bool *failed)
{
    if (*failed || off <= 0) {
        return 0;
    }
    if (httpd_resp_send_chunk(req, chunk, off) != ESP_OK) {
        *failed = true;
    }
    return 0;
}

// The log ring as a text file.
//
// Chunked rather than one buffer: 128 lines at up to ~193 characters is ~24KB,
// six times the largest response this server builds, and logs_get_handler()'s
// 4096-byte static is already the ceiling this stack tolerates alongside OTA.
//
// Every decision that can fail happens before the first chunk goes out. Once
// httpd_resp_send_chunk() has written a byte the status line is gone, and a 500
// raised halfway through becomes a truncated file that the browser saves without
// complaint - so from that point on a send failure can only be abandoned, not
// reported.
static esp_err_t logs_download_get_handler(httpd_req_t *req)
{
    if (!check_admin_auth(req)) {
        send_auth_error(req);
        return ESP_OK;
    }
    if (auth_cfg_password_is_default()) {
        send_setup_required(req);
        return ESP_OK;
    }

    reset_log_entry_t boot;
    bool have_boot = reset_log_get_current(&boot);

    eth_link_status_t eth;
    eth_link_get_status(&eth);

    syslog_cfg_t sys_cfg;
    syslog_cfg_get(&sys_cfg);
    syslog_status_t sys_st;
    syslog_get_status(&sys_st);

    uint32_t oldest = 0, newest = 0, missed = 0;
    log_buf_get_range(&oldest, &newest, &missed);

    const esp_app_desc_t *app_desc = esp_app_get_description();
    uint64_t uptime_s = (uint64_t)(esp_timer_get_time() / 1000000);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    // No date in the filename, because this device has no clock and will not
    // guess at one. The boot number and the uptime are what it does know: two
    // downloads from one boot sort correctly and never collide, and the name
    // says what the file is relative to without claiming when it was taken.
    char disp[96];
    snprintf(disp, sizeof(disp),
             "attachment; filename=\"wt32-bridge-log-boot%u-%llus.txt\"",
             have_boot ? (unsigned)boot.boot_seq : 0u,
             (unsigned long long)uptime_s);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    static char chunk[LOGS_DL_CHUNK];
    bool failed = false;
    int off = 0;

    // The header block. A downloaded file is read somewhere the device is not,
    // so everything needed to interpret the lines below travels with them - and
    // the wording is deliberately the same the /resets page and the console use,
    // so the three never disagree about one boot.
    off = resp_append(chunk, sizeof(chunk), off,
                      "wt32-bridge device log\n\n"
                      "version         %s\n", app_desc->version);
    if (have_boot) {
        off = resp_append(chunk, sizeof(chunk), off,
                          "boot            %u (%s, intent %s)\n",
                          (unsigned)boot.boot_seq,
                          reset_log_reason_name(boot.reason),
                          reset_log_intent_name(boot.intent));
    } else {
        off = resp_append(chunk, sizeof(chunk), off, "boot            unknown\n");
    }
    off = resp_append(chunk, sizeof(chunk), off,
                      "uptime          %llu s\n"
                      "ethernet        %s%s\n",
                      (unsigned long long)uptime_s,
                      eth.up ? "up" : "down",
                      eth.up ? "" : " (no link)");
    if (eth.up) {
        off = resp_append(chunk, sizeof(chunk), off,
                          "                %u Mbit, %s duplex, %u flap(s), %u s in this state\n",
                          (unsigned)eth.speed_mbit, eth.full_duplex ? "full" : "half",
                          (unsigned)eth.flaps, (unsigned)eth.since_change_s);
    }
    off = dl_flush(req, chunk, off, &failed);

    off = resp_append(chunk, sizeof(chunk), 0,
                      "log ring        %u lines, holding %u..%u\n"
                      "lines lost      %u the capture hook could not store\n"
                      "capture level   %s (build maximum: %s)\n",
                      (unsigned)LOG_BUF_LINES, (unsigned)oldest, (unsigned)newest,
                      (unsigned)missed,
                      log_level_name(esp_log_level_get("*")),
                      log_level_name((esp_log_level_t)CONFIG_LOG_MAXIMUM_LEVEL));
    if (sys_cfg.enabled) {
        char server[16];
        esp_ip4_addr_t addr = { .addr = sys_cfg.server_ip };
        esp_ip4addr_ntoa(&addr, server, sizeof(server));
        off = resp_append(chunk, sizeof(chunk), off,
                          "syslog          enabled, %s:%u, %u sent, %u aged out, %u failed\n",
                          server, (unsigned)sys_cfg.port, (unsigned)sys_st.sent,
                          (unsigned)sys_st.aged_out, (unsigned)sys_st.send_fail);
    } else {
        off = resp_append(chunk, sizeof(chunk), off, "syslog          disabled\n");
    }
    off = resp_append(chunk, sizeof(chunk), off,
                      "timestamps      milliseconds since this boot started, deliberately, even\n"
                      "                when the device does know the wall-clock time. There is no\n"
                      "                RTC battery here, so a clock exists only while the WAN can\n"
                      "                reach an NTP server - and a log whose lines are relative\n"
                      "                for the first part of a boot and absolute afterwards is\n"
                      "                harder to read than one that is consistently relative. The\n"
                      "                reboot history at /resets IS dated where it can be.\n"
                      "\n");
    // Built from the same field widths as LOGS_DL_LINE_FMT rather than written
    // out as a literal, so the two cannot drift apart - the first attempt was a
    // literal and put "tag" two columns right of the tags underneath it. The
    // level column is one character wide, so its heading has to be too.
    off = resp_append(chunk, sizeof(chunk), off, "%6s  %8s  %c  %-24s  %s\n",
                      "seq", "ms", 'L', "tag", "message");
    off = dl_flush(req, chunk, off, &failed);

    // Lines that aged out before the first one this file carries. Reported the
    // same way logs_get_handler() reports "lost", and for the same reason: a
    // hole spliced over silently is worse than a hole named.
    uint32_t gap = 0;

    for (uint32_t seq = oldest; seq <= newest && !failed; seq++) {
        char level = '?';
        uint32_t ts_ms = 0;
        char tag[LOG_BUF_TAG_MAX];
        char msg[LOG_BUF_MSG_MAX];

        // Copied out before formatting, and never while holding the ring's
        // lock - httpd_resp_send_chunk() blocks on a socket, and resp_append()
        // logs on truncation. Either one under that lock is a deadlock against
        // this same handler.
        if (!log_buf_get_line(seq, &level, &ts_ms, tag, sizeof(tag), msg, sizeof(msg))) {
            gap++;
            continue;
        }

        if ((size_t)off + LOGS_DL_LINE_MAX >= sizeof(chunk)) {
            off = dl_flush(req, chunk, off, &failed);
            if (failed) {
                break;
            }
        }

        if (gap > 0) {
            // Deliberately unlike a data row, so nothing parsing this file by
            // column mistakes it for one.
            off = resp_append(chunk, sizeof(chunk), off,
                              "*** %u line(s) were overwritten while this file was being written ***\n",
                              (unsigned)gap);
            gap = 0;
        }

        off = resp_append(chunk, sizeof(chunk), off, LOGS_DL_LINE_FMT,
                          (unsigned)seq, (unsigned)ts_ms, level,
                          tag[0] != '\0' ? tag : "-", msg);
    }

    if (gap > 0 && !failed) {
        if ((size_t)off + LOGS_DL_LINE_MAX >= sizeof(chunk)) {
            off = dl_flush(req, chunk, off, &failed);
        }
        off = resp_append(chunk, sizeof(chunk), off,
                          "*** %u line(s) were overwritten while this file was being written ***\n",
                          (unsigned)gap);
    }
    off = dl_flush(req, chunk, off, &failed);

    // The empty chunk that terminates the response. Sent even after a failure:
    // it is what closes the transfer, and there is nothing else left to say.
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
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
    //
    // 24 rather than 20: the three syslog/download routes took the count to 19,
    // which is not the headroom this comment asks for - one more route and the
    // failure is silent. Each slot is a pointer.
    //
    // 28 rather than 24: the two /api/wan routes take the count to 22, which is
    // again not the headroom this comment asks for.
    config.max_uri_handlers = 28;

    // Without this the server stops listening the moment every session slot is
    // taken: httpd_server() only adds listen_fd to the select set when a slot is
    // free, so a full server does not refuse connections so much as stop hearing
    // them, and the client sits there until it times out. The dashboard polls
    // three endpoints a second over keep-alive, and a browser opens up to six
    // connections per host, so it reaches that state on its own within seconds -
    // measured: with four connections held, every further request failed to
    // connect at all, and all four recovered the instant they were released.
    // From the page it reads as "Lost connection to bridge", which is what sent
    // this looking at the network first.
    //
    // With it, a full server closes its least recently used session to admit the
    // new connection. Idle keep-alive sockets are exactly what that picks off.
    config.lru_purge_enable = true;
    // Purging still costs a connection, so leave enough slots that an ordinary
    // one or two tabs never trigger it. The cap is CONFIG_LWIP_MAX_SOCKETS - 3
    // (httpd_main.c reserves three for the listener, the control socket pair,
    // and accept headroom), so this and the sdkconfig bump go together -
    // raising this alone makes httpd_start() fail with ESP_ERR_INVALID_ARG.
    //
    // That cap is a floor, not a budget: it says httpd *can* start, not that
    // anything is left over. At 13 this server alone can hold all 16 descriptors
    // the pool used to have, and dhcp_server.c has held one of them since before
    // this comment - so the pool was already oversubscribed, silently, because
    // 13 concurrent sessions is rare. lru_purge_enable above does not cover it:
    // it fires when the *session table* fills, not when lwIP has no descriptor
    // left to accept with, so the symptom is the same "server goes deaf" this
    // file already fixed once by another route. CONFIG_LWIP_MAX_SOCKETS is 18
    // for that reason - 16 for httpd, one for the DHCP server, one for syslog.c.
    config.max_open_sockets = 13;

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
    const httpd_uri_t time_get_uri = {.uri = "/api/time", .method = HTTP_GET, .handler = time_get_handler};
    const httpd_uri_t time_post_uri = {.uri = "/api/time", .method = HTTP_POST, .handler = time_post_handler};
    const httpd_uri_t clients_uri = {.uri = "/api/clients", .method = HTTP_GET, .handler = clients_get_handler};
    const httpd_uri_t history_uri = {.uri = "/api/client/history", .method = HTTP_GET, .handler = client_history_get_handler};
    const httpd_uri_t kick_uri = {.uri = "/api/clients/kick", .method = HTTP_POST, .handler = kick_post_handler};
    const httpd_uri_t system_uri = {.uri = "/api/system", .method = HTTP_GET, .handler = system_get_handler};
    const httpd_uri_t ota_uri = {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_post_handler};
    const httpd_uri_t leases_page_uri = {.uri = "/leases", .method = HTTP_GET, .handler = leases_page_get_handler};
    const httpd_uri_t leases_uri = {.uri = "/api/leases", .method = HTTP_GET, .handler = leases_get_handler};
    const httpd_uri_t resets_page_uri = {.uri = "/resets", .method = HTTP_GET, .handler = resets_page_get_handler};
    const httpd_uri_t resets_uri = {.uri = "/api/resets", .method = HTTP_GET, .handler = resets_get_handler};
    const httpd_uri_t logs_page_uri = {.uri = "/logs", .method = HTTP_GET, .handler = logs_page_get_handler};
    const httpd_uri_t logs_uri = {.uri = "/api/logs", .method = HTTP_GET, .handler = logs_get_handler};
    const httpd_uri_t logs_level_uri = {.uri = "/api/logs/level", .method = HTTP_POST, .handler = logs_level_post_handler};
    const httpd_uri_t logs_dl_uri = {.uri = "/api/logs/download", .method = HTTP_GET, .handler = logs_download_get_handler};
    const httpd_uri_t syslog_get_uri = {.uri = "/api/syslog", .method = HTTP_GET, .handler = syslog_get_handler};
    const httpd_uri_t syslog_post_uri = {.uri = "/api/syslog", .method = HTTP_POST, .handler = syslog_post_handler};
    const httpd_uri_t wan_page_uri = {.uri = "/wan", .method = HTTP_GET, .handler = wan_page_get_handler};
    const httpd_uri_t lan_page_uri = {.uri = "/lan", .method = HTTP_GET, .handler = lan_page_get_handler};
    const httpd_uri_t wan_get_uri = {.uri = "/api/wan", .method = HTTP_GET, .handler = wan_get_handler};
    const httpd_uri_t wan_post_uri = {.uri = "/api/wan", .method = HTTP_POST, .handler = wan_post_handler};

    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &admin_page_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &wifi_uri);
    httpd_register_uri_handler(server, &admin_uri);
    httpd_register_uri_handler(server, &clients_uri);
    httpd_register_uri_handler(server, &history_uri);
    httpd_register_uri_handler(server, &kick_uri);
    httpd_register_uri_handler(server, &system_uri);
    httpd_register_uri_handler(server, &ota_uri);
    httpd_register_uri_handler(server, &leases_page_uri);
    httpd_register_uri_handler(server, &leases_uri);
    httpd_register_uri_handler(server, &resets_page_uri);
    httpd_register_uri_handler(server, &resets_uri);
    httpd_register_uri_handler(server, &logs_page_uri);
    httpd_register_uri_handler(server, &logs_uri);
    httpd_register_uri_handler(server, &logs_level_uri);
    // Registered before the wildcard-free /api/syslog pair only for readability;
    // /api/logs/download is a distinct literal, so no ordering is load-bearing.
    httpd_register_uri_handler(server, &logs_dl_uri);
    httpd_register_uri_handler(server, &syslog_get_uri);
    httpd_register_uri_handler(server, &syslog_post_uri);
    httpd_register_uri_handler(server, &wan_page_uri);
    httpd_register_uri_handler(server, &lan_page_uri);
    httpd_register_uri_handler(server, &wan_get_uri);
    httpd_register_uri_handler(server, &wan_post_uri);
    httpd_register_uri_handler(server, &time_get_uri);
    httpd_register_uri_handler(server, &time_post_uri);

    ESP_LOGI(TAG, "Web server started");
    return server;
}
