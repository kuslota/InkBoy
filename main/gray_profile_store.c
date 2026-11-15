#include "gray_profile_store.h"
#include <string.h>

esp_err_t gray_profile_nvs_get(nvs_handle_t h, const char *key, gray_profile_t *profile)
{
    if (!key || !profile) return ESP_ERR_INVALID_ARG;
    gray_profile_t stored;
    size_t size=sizeof(stored);
    esp_err_t error=nvs_get_blob(h,key,&stored,&size);
    if (error) return error;
    if (size!=sizeof(stored) || !gray_profile_valid(&stored)) return ESP_ERR_INVALID_ARG;
    *profile=stored;
    return ESP_OK;
}

esp_err_t gray_profile_nvs_set(nvs_handle_t h, const char *key, const gray_profile_t *profile)
{
    if (!key || !gray_profile_valid(profile)) return ESP_ERR_INVALID_ARG;
    /* One blob prevents a restored preset becoming a mixture of settings. */
    return nvs_set_blob(h,key,profile,sizeof(*profile));
}
