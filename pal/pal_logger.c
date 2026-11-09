/*
 * pal_logger.c
 *
 * Pico port of the Infineon NEW_PAL_TEMPLATE pal_logger.c.
 *
 * Log output goes to stdio (UART0 @ 115200 on the bench). The library's
 * own logger (src/common/optiga_lib_logger.c) declares
 * `extern pal_logger_t logger_console` — the PAL must provide the
 * object, as done here.
 */

#include <stdio.h>

#include "pal_logger.h"

/* Platform-provided logger context (declared extern by the library). */
pal_logger_t logger_console = {
    .logger_config_ptr = NULL,
    .logger_rx_flag = 0,
    .logger_tx_flag = 1,
};

pal_status_t pal_logger_init(void *p_logger_context) {
    (void)p_logger_context;
    return PAL_STATUS_SUCCESS;
}

pal_status_t pal_logger_deinit(void *p_logger_context) {
    (void)p_logger_context;
    return PAL_STATUS_SUCCESS;
}

pal_status_t
pal_logger_write(void *p_logger_context, const uint8_t *p_log_data, uint32_t log_data_length) {
    (void)p_logger_context;
    if ((NULL != p_log_data) && (0 < log_data_length)) {
        // The data is not necessarily NUL-terminated: write raw bytes.
        (void)fwrite(p_log_data, 1, log_data_length, stdout);
    }
    return PAL_STATUS_SUCCESS;
}

pal_status_t
pal_logger_read(void *p_logger_context, uint8_t *p_log_data, uint32_t log_data_length) {
    (void)p_logger_context;
    (void)p_log_data;
    (void)log_data_length;
    return PAL_STATUS_FAILURE;
}
