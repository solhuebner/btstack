#include <assert.h>
#include <stdatomic.h>
#include "hal_uart_dma.h"

#include "btstack_defines.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#define UART_NO                  (CONFIG_BTSTACK_UART_NUM)
#define UART_BUF_SZ              (1024)

#define UART_TX_PIN              (CONFIG_BTSTACK_UART_TX_PIN)
#define UART_RX_PIN              (CONFIG_BTSTACK_UART_RX_PIN)
#define UART_RTS_PIN             (CONFIG_BTSTACK_UART_RTS_PIN)
#define UART_CTS_PIN             (CONFIG_BTSTACK_UART_CTS_PIN)
#define UART_NRESET              (CONFIG_BTSTACK_UART_NRESET_PIN)

// Manually control RTS / data from Controller
#define ENABLE_UART_MANUAL_RTS

// report blocking writes
// #define ENABLE_UART_REPORT_TX_DELAY

// Without manual RTS, incoming data gets lost without an error event on ESP32-P4

static uart_config_t uart_config = {
    .baud_rate  = 1000000,
    .data_bits  = UART_DATA_8_BITS,
    .parity     = UART_PARITY_DISABLE,
    .stop_bits  = UART_STOP_BITS_1,
    .flow_ctrl  = UART_HW_FLOWCTRL_CTS_RTS,
    .source_clk = UART_SCLK_DEFAULT,
};

typedef void (*callback_t)();
static callback_t receive_callback;
static callback_t send_callback;
static QueueHandle_t uart_queue;
static SemaphoreHandle_t rx_mutex;
static void uart_event_task(void *pvParameters);
static const char *TAG = "hal_uart";

/**
 * @brief Init and open device
 */
void hal_uart_dma_init(void) {

#if defined(UART_NRESET) && (UART_NRESET >= 0)
    // Configure GPIO15 as output
    gpio_config_t io_conf_nreset = {
        .pin_bit_mask = (1ULL<<UART_NRESET),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf_nreset);

    // Set NRESET to LOW
    ESP_LOGI(TAG, "nRESET: LOW");
    gpio_set_level(UART_NRESET, 0);

    // wait for 100 ms
    vTaskDelay(100 / portTICK_PERIOD_MS);

    // Set NRESET to HIGH
    ESP_LOGI(TAG, "nRESET: HIGH");
    gpio_set_level(UART_NRESET, 1);

    // wait for 100 ms
    vTaskDelay(100 / portTICK_PERIOD_MS);
#endif

    int intr_alloc_flags = 0;
#ifdef CONFIG_UART_ISR_IN_IRAM
    intr_alloc_flags = ESP_INTR_FLAG_IRAM;
#endif

    rx_mutex = xSemaphoreCreateMutex();
    assert(rx_mutex != NULL);

    ESP_ERROR_CHECK(uart_driver_install(UART_NO, UART_BUF_SZ * 2, UART_BUF_SZ * 2, 20, &uart_queue, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(UART_NO, &uart_config));

#ifdef ENABLE_UART_MANUAL_RTS
    // Configure GPIO15 as output
    gpio_config_t io_conf_rts = {
        .pin_bit_mask = (1ULL<<UART_RTS_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf_rts);

    // Set RTS to HIGH
    gpio_set_level(UART_RTS_PIN, 1);

    // Configure UART - we control RTS
    ESP_ERROR_CHECK(uart_set_pin(UART_NO, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_CTS_PIN));

#else

#ifdef CONFIG_EXAMPLE_HCI_UART_INVERT_RTS

    // On ESP32-P4, RTS is HIGH when we're ready to receive
    // this is opposite to common practice but can be fixed by inverting the signal

    // Has not been tested on other ESP32 chips other then ESP32-P4

    uint32_t invert_mask = 0;
    invert_mask |= UART_SIGNAL_RTS_INV;
    ESP_ERROR_CHECK(uart_set_line_inverse(UART_NO, invert_mask));
#endif

    // Configure UART - UART controls RTS
    ESP_ERROR_CHECK(uart_set_pin(UART_NO, UART_TX_PIN, UART_RX_PIN, UART_RTS_PIN, UART_CTS_PIN));

#endif

    //Create a task to handler UART event from ISR
    xTaskCreate(uart_event_task, "uart_event_task", 3072, NULL, 10, NULL);
}

typedef struct {
    uint8_t *buf;
    uint16_t nbytes;
} io_cb_t;

static io_cb_t current_transfer;

static void IRAM_ATTR uart_event_task(void *pvParameters)
{
    // flush queue
    uart_flush_input(UART_NO);
    xQueueReset(uart_queue);

    uart_event_t event;
    for (;;) {
        //Waiting for UART event.
        if (xQueueReceive(uart_queue, (void *)&event, (TickType_t)portMAX_DELAY)) {
//            ESP_LOGI(TAG, "uart[%d] event:", UART_NO);
            switch (event.type) {
            //Event of UART receiving data
            /*We'd better handler data event fast, there would be much more data events than
            other types of events. If we take too much time on data event, the queue might
            be full.*/
            case UART_DATA:
                if( xSemaphoreTake(rx_mutex, portMAX_DELAY) == pdTRUE ) {
                    size_t cached_data_len = 0;
                    uart_get_buffered_data_len(UART_NO, &cached_data_len);
                    bool transfer_complete = (current_transfer.buf != NULL) && (cached_data_len >= current_transfer.nbytes);
                    // ESP_LOGI(TAG, "[UART DATA]: new %d => cached %u, requested %d -> completed %u", event.size, cached_data_len, current_transfer.nbytes, transfer_complete);
                    if( transfer_complete ) {
                        int length = uart_read_bytes(UART_NO, current_transfer.buf, current_transfer.nbytes, portMAX_DELAY);
                        assert(length == current_transfer.nbytes);
                        current_transfer = (io_cb_t){ .buf = NULL, .nbytes = 0 };
                    }
                    xSemaphoreGive(rx_mutex);
                    if( transfer_complete && (receive_callback != NULL )) {
#ifdef ENABLE_UART_MANUAL_RTS
                        // Set GPIO15 to LOW
                        gpio_set_level(UART_RTS_PIN, 1);
#endif

                        // start processing
                        receive_callback();
                    }
                } else {
                    assert(false);
                }
                break;
            //Event of HW FIFO overflow detected
            case UART_FIFO_OVF:
                ESP_LOGE(TAG, "hw fifo overflow");
                // If fifo overflow happened, you should consider adding flow control for your application.
                // The ISR has already reset the rx FIFO,
                // As an example, we directly flush the rx buffer here in order to read more data.
                uart_flush_input(UART_NO);
                xQueueReset(uart_queue);
                break;
            //Event of UART ring buffer full
            case UART_BUFFER_FULL:
                ESP_LOGE(TAG, "ring buffer full");
                // If buffer full happened, you should consider increasing your buffer size
                // As an example, we directly flush the rx buffer here in order to read more data.
                uart_flush_input(UART_NO);
                xQueueReset(uart_queue);
                break;
            //Event of UART RX break detected
            case UART_BREAK:
                ESP_LOGE(TAG, "uart rx break");
                break;
            //Event of UART parity check error
            case UART_PARITY_ERR:
                ESP_LOGE(TAG, "uart parity error");
                break;
            //Event of UART frame error
            case UART_FRAME_ERR:
                ESP_LOGE(TAG, "uart frame error");
                break;
            //Others
            default:
                ESP_LOGI(TAG, "uart event type: %d", event.type);
                break;
            }
        }
    }
    vTaskDelete(NULL);
}

/**
 * @brief Set callback for block received - can be called from ISR context
 * @param callback
 */
void hal_uart_dma_set_block_received( void (*callback)(void)) {
    receive_callback = callback;
}

/**
 * @brief Set callback for block sent - can be called from ISR context
 * @param callback
 */
void hal_uart_dma_set_block_sent( void (*callback)(void)) {
    send_callback = callback;
}

void hal_uart_dma_set_csr_irq_handler( void (*csr_irq_handler)(void)){
    UNUSED(csr_irq_handler);
}

void hal_uart_dma_set_sleep(uint8_t sleep) {
    UNUSED(sleep);
}

/**
 * @brief Receive block. When done, callback set by hal_uart_dma_set_block_received must be called
 * @param buffer
 * @param lengh
 */
void hal_uart_dma_receive_block(uint8_t *buffer, uint16_t len) {
    assert(current_transfer.buf == NULL);
    assert(current_transfer.nbytes == 0);

    if( xSemaphoreTake(rx_mutex, portMAX_DELAY) == pdTRUE ) {
        size_t cached_data_len = 0;
        uart_get_buffered_data_len(UART_NO, &cached_data_len);
        bool rx_complete = cached_data_len >= len;
        // ESP_LOGI(TAG, "[UART RECV]: %u), have %u -> complete %u", len, cached_data_len, rx_complete);
        if( rx_complete ) {
            int length = uart_read_bytes(UART_NO, buffer, len, portMAX_DELAY);
            assert(length == len);
        } else {
            current_transfer.buf = buffer;
            current_transfer.nbytes = len;
        }
        xSemaphoreGive(rx_mutex);
        if( rx_complete && (receive_callback != NULL )) {
            receive_callback();
        }
#ifdef ENABLE_UART_MANUAL_RTS
        else {
            // Set GPIO15 to LOW
            gpio_set_level(UART_RTS_PIN, 0);
        }
#endif
    } else {
        assert(false);
    }
}

/**
 * @brief Set baud rate
 * @note During baud change, TX line should stay high and no data should be received on RX accidentally
 * @param baudrate
 */
int  hal_uart_dma_set_baud(uint32_t baud) {
    uart_config.baud_rate = baud;
    ESP_ERROR_CHECK(uart_param_config(UART_NO, &uart_config));
    return 0;
}

#ifdef HAVE_UART_DMA_SET_FLOWCONTROL
/**
 * @brief Set flowcontrol
 * @param flowcontrol enabled
 */
int  hal_uart_dma_set_flowcontrol(int flowcontrol) {
    if( flowcontrol == BTSTACK_UART_FLOWCONTROL_ON ) {
        uart_config.flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS;
    } else {
        uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    }
    ESP_ERROR_CHECK(uart_param_config(UART_NO, &uart_config));
    return 0;
}
#endif

/**
 * @brief Send block. When done, callback set by hal_uart_set_block_sent must be called
 * @param buffer
 * @param lengh
 */
void hal_uart_dma_send_block(const uint8_t *buffer, uint16_t len) {
    uint8_t *p = (uint8_t*)buffer;
    int len_write = 0;

#ifdef ENABLE_UART_REPORT_TX_DELAY
    int64_t start = esp_timer_get_time();
#endif

    while (len) {
        len_write = uart_write_bytes(UART_NO, p, len);
        assert(len_write > 0);
        len -= len_write;
        p += len_write;
    }

#ifdef ENABLE_UART_REPORT_TX_DELAY
    int64_t end = esp_timer_get_time();

    if (end - start > 1000) {
        printf("uart_write_bytes took %lld us\n", (end - start));
    }
#endif

    if( send_callback != NULL ) {
        send_callback();
    }
}
