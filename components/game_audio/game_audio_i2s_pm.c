#include "game_audio_i2s_pm.h"

#include "sdkconfig.h"

#if CONFIG_PM_ENABLE
#include "esp_idf_version.h"
#include "i2s_private.h"

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 1, 0) || ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 2, 0)
#error "i2s_private.h is not API: recheck pm_lock use in i2s_common.c and i2s_std.c, then widen this range"
#endif
#if !SOC_I2S_HW_VERSION_2
#error "I2S HW v1 clocks DMA and I2S from different domains; IDF keeps APB fixed there"
#endif

/* IDF 6.1 holds APB_FREQ_MAX for every I2S clock except APLL, which the S3 lacks. A crystal-clocked
 * channel is just as independent of the CPU clock, so it gets the lock type IDF uses for APLL. */
esp_err_t game_audio_i2s_allow_dfs(i2s_chan_handle_t channel)
{
    esp_pm_lock_handle_t lock;
    esp_err_t err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "i2s_xtal", &lock);
    if (err != ESP_OK) return err;
    xSemaphoreTake(channel->mutex, portMAX_DELAY);
    if (channel->state != I2S_CHAN_STATE_READY || channel->clk_src != I2S_CLK_SRC_XTAL || !channel->pm_lock) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        err = esp_pm_lock_delete(channel->pm_lock);
    }
    if (err == ESP_OK) channel->pm_lock = lock;
    xSemaphoreGive(channel->mutex);
    if (err != ESP_OK) (void)esp_pm_lock_delete(lock);
    return err;
}
#else
esp_err_t game_audio_i2s_allow_dfs(i2s_chan_handle_t channel)
{
    (void)channel;
    return ESP_OK;
}
#endif
