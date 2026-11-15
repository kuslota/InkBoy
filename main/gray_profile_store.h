#pragma once
#include "gray_profile.h"
#include "esp_err.h"
#include "nvs.h"

/* Handle-based access leaves the namespace and the commit to the caller. */
esp_err_t gray_profile_nvs_get(nvs_handle_t h, const char *key, gray_profile_t *profile);
esp_err_t gray_profile_nvs_set(nvs_handle_t h, const char *key, const gray_profile_t *profile);
