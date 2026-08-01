#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUTH_CFG_USERNAME_MAX_LEN 32   // includes NUL
#define AUTH_CFG_PASSWORD_MAX_LEN 64   // includes NUL

// Must be called once (after the FreeRTOS scheduler is running, before any
// web server or console access) to set up the credential RAM cache.
esp_err_t auth_cfg_init(void);

// Loads the admin username/password from a RAM cache (populated from NVS
// on first use, or from compiled-in defaults - "admin"/"admin" - if
// nothing has been saved yet). username buffer must be at least
// AUTH_CFG_USERNAME_MAX_LEN bytes, password buffer at least
// AUTH_CFG_PASSWORD_MAX_LEN bytes.
void auth_cfg_load(char *username, char *password);

// Returns true if the current admin username AND password both still
// equal the compiled-in defaults (AUTH_CFG_DEFAULT_USERNAME/PASSWORD).
bool auth_cfg_is_default(void);

// Persists a new admin username/password to NVS.
esp_err_t auth_cfg_save(const char *username, const char *password);

// Validates a candidate admin username (1-31 bytes) and password (4-63
// bytes). Returns true and leaves *err_msg untouched on success; returns
// false and sets *err_msg to a static, human-readable reason on failure.
bool auth_cfg_validate(const char *username, const char *password, const char **err_msg);

#ifdef __cplusplus
}
#endif
