#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUTH_CFG_USERNAME_MAX_LEN 32   // includes NUL
#define AUTH_CFG_PASSWORD_MAX_LEN 64   // includes NUL

// Loads the admin username/password from NVS, falling back to compiled-in
// defaults ("admin"/"admin") if nothing has been saved yet. username
// buffer must be at least AUTH_CFG_USERNAME_MAX_LEN bytes, password buffer
// at least AUTH_CFG_PASSWORD_MAX_LEN bytes.
void auth_cfg_load(char *username, char *password);

// Persists a new admin username/password to NVS.
esp_err_t auth_cfg_save(const char *username, const char *password);

// Validates a candidate admin username (1-31 bytes) and password (4-63
// bytes). Returns true and leaves *err_msg untouched on success; returns
// false and sets *err_msg to a static, human-readable reason on failure.
bool auth_cfg_validate(const char *username, const char *password, const char **err_msg);

#ifdef __cplusplus
}
#endif
