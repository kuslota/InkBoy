#pragma once

#include "driver/i2s_types.h"
#include "esp_err.h"

/* Lets DFS go below 80 MHz while the channel runs. Call between init and the first enable on
 * an I2S_CLK_SRC_XTAL channel; on error the driver's own lock stays and nothing else changes. */
esp_err_t game_audio_i2s_allow_dfs(i2s_chan_handle_t channel);
