#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SPI2_HOST 1
#define SPI_DMA_CH_AUTO 3

typedef struct host_spi_device *spi_device_handle_t;
typedef struct { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num; int max_transfer_sz; } spi_bus_config_t;
typedef struct { int clock_speed_hz, mode, spics_io_num, queue_size; } spi_device_interface_config_t;
typedef struct { size_t length; const void *tx_buffer; unsigned override_freq_hz; } spi_transaction_t;

esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *config, int dma);
esp_err_t spi_bus_free(int host);
esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *config, spi_device_handle_t *handle);
esp_err_t spi_bus_remove_device(spi_device_handle_t handle);
esp_err_t spi_device_polling_transmit(spi_device_handle_t handle, spi_transaction_t *transaction);
esp_err_t spi_device_transmit(spi_device_handle_t handle, spi_transaction_t *transaction);
