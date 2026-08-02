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

// Returns true if the admin password is still the compiled-in default,
// meaning the device has never been set up and anyone who can reach it
// knows the credentials. The username is deliberately not part of this:
// it isn't a secret, so renaming it protects nothing.
bool auth_cfg_password_is_default(void);

// Persists a new admin username/password to NVS.
esp_err_t auth_cfg_save(const char *username, const char *password);

// Validates a candidate admin username (1-31 bytes) and password (4-63
// bytes, and not the compiled-in default). Returns true and leaves
// *err_msg untouched on success; returns false and sets *err_msg to a
// static, human-readable reason on failure.
bool auth_cfg_validate(const char *username, const char *password, const char **err_msg);

#ifdef __cplusplus
}
#endif
