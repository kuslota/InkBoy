#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "host_fakes.h"
#include <stdlib.h>

host_spi_transaction_t host_spi_last;
unsigned host_spi_transactions;
unsigned host_spi_polling_transactions, host_spi_interrupt_transactions;
esp_err_t host_spi_error;

esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *config, int dma) { (void)host; (void)config; (void)dma; return ESP_OK; }
esp_err_t spi_bus_free(int host) { (void)host; return ESP_OK; }

esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *config, spi_device_handle_t *handle)
{
    (void)host; (void)config;
    *handle = (spi_device_handle_t)calloc(1, 1);
    return ESP_OK;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t handle) { free(handle); return ESP_OK; }

esp_err_t spi_device_polling_transmit(spi_device_handle_t handle, spi_transaction_t *transaction)
{
    (void)handle;
    ++host_spi_transactions;
    ++host_spi_polling_transactions;
    host_spi_last = (host_spi_transaction_t){transaction->length, transaction->tx_buffer, transaction->override_freq_hz};
    if (!host_spi_error) host_ssd1681_spi(transaction->tx_buffer, transaction->length / 8, false);
    return host_spi_error;
}

esp_err_t spi_device_transmit(spi_device_handle_t handle, spi_transaction_t *transaction)
{
    (void)handle;
    ++host_spi_transactions;
    ++host_spi_interrupt_transactions;
    host_spi_last = (host_spi_transaction_t){transaction->length, transaction->tx_buffer, transaction->override_freq_hz};
    if (!host_spi_error) host_ssd1681_spi(transaction->tx_buffer, transaction->length / 8, true);
    return host_spi_error;
}

esp_err_t gpio_set_direction(gpio_num_t pin, int mode) { (void)pin; (void)mode; return ESP_OK; }
