#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "driver/uart.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

#define UART_PORT           UART_NUM_0
#define UART_BAUD_RATE      115200
#define UART_RX_BUF_SIZE    1024

#define MAX_DEVICES         128
#define MAX_NAME_LEN        32
#define REPORT_INTERVAL_US  (1000LL * 1000LL)       // Min 1s between reports per MAC
#define STALE_TIMEOUT_US    (15LL * 1000LL * 1000LL) // Drop devices unseen for 15s

typedef struct {
    bool     in_use;
    uint8_t  addr[6];
    int8_t   rssi;
    char     name[MAX_NAME_LEN + 1];
    bool     has_company_id;
    uint16_t company_id;
    bool     has_service_uuid16;
    uint16_t service_uuid16;
    int64_t  last_seen_us;
    int64_t  last_reported_us;
} ble_device_t;

static ble_device_t      s_devices[MAX_DEVICES];
static SemaphoreHandle_t s_dev_mutex;
static SemaphoreHandle_t s_uart_mutex;
static uint8_t           s_own_addr_type;

static void start_ble_scan(void);

static void uart_send_line(const char *line) {
    if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uart_write_bytes(UART_PORT, line, strlen(line));
        xSemaphoreGive(s_uart_mutex);
    }
}

/* Strip control characters so advertised names stay printable ASCII */
static void sanitize_name(char *dst, const uint8_t *src, uint8_t len) {
    size_t n = (len < MAX_NAME_LEN) ? len : MAX_NAME_LEN;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = src[i];
        dst[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : ' ';
    }
    dst[n] = '\0';
}

/* JSON-escape quotes and backslashes */
static void json_escape(char *dst, size_t dst_sz, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 2 < dst_sz; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            dst[j++] = '\\';
        }
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

static void emit_device_json(const ble_device_t *d) {
    char buf[256];
    char esc_name[MAX_NAME_LEN * 2 + 1];
    const uint8_t *a = d->addr;

    // NimBLE stores Bluetooth addresses in little-endian byte order
    int off = snprintf(
        buf, sizeof(buf),
        "{\"event\":\"detection\",\"protocol\":\"ble\","
        "\"mac_address\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"rssi\":%d",
        a[5], a[4], a[3], a[2], a[1], a[0], (int)d->rssi
    );

    if (d->name[0] != '\0' && off > 0 && (size_t)off < sizeof(buf)) {
        json_escape(esc_name, sizeof(esc_name), d->name);
        off += snprintf(buf + off, sizeof(buf) - off, ",\"local_name\":\"%s\"", esc_name);
    }
    if (d->has_company_id && off > 0 && (size_t)off < sizeof(buf)) {
        off += snprintf(buf + off, sizeof(buf) - off, ",\"company_id\":%u", (unsigned)d->company_id);
    }
    if (d->has_service_uuid16 && off > 0 && (size_t)off < sizeof(buf)) {
        off += snprintf(buf + off, sizeof(buf) - off, ",\"service_uuid\":%u", (unsigned)d->service_uuid16);
    }
    if (off > 0 && (size_t)off + 3 < sizeof(buf)) {
        snprintf(buf + off, sizeof(buf) - off, "}\n");
        uart_send_line(buf);
    }
}

static void upsert_device(const uint8_t addr[6], int8_t rssi, const char *name,
                          bool has_cid, uint16_t cid, bool has_uuid16, uint16_t uuid16) {
    int64_t now = esp_timer_get_time();

    if (xSemaphoreTake(s_dev_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    int free_idx = -1;
    int oldest_idx = 0;
    int64_t oldest_time = INT64_MAX;

    for (int i = 0; i < MAX_DEVICES; i++) {
        if (s_devices[i].in_use) {
            if (memcmp(s_devices[i].addr, addr, 6) == 0) {
                s_devices[i].rssi = rssi;
                s_devices[i].last_seen_us = now;
                if (name[0] != '\0') {
                    strncpy(s_devices[i].name, name, MAX_NAME_LEN);
                    s_devices[i].name[MAX_NAME_LEN] = '\0';
                }
                if (has_cid) {
                    s_devices[i].has_company_id = true;
                    s_devices[i].company_id = cid;
                }
                if (has_uuid16) {
                    s_devices[i].has_service_uuid16 = true;
                    s_devices[i].service_uuid16 = uuid16;
                }
                xSemaphoreGive(s_dev_mutex);
                return;
            }
            if (s_devices[i].last_seen_us < oldest_time) {
                oldest_time = s_devices[i].last_seen_us;
                oldest_idx = i;
            }
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }

    int target = (free_idx >= 0) ? free_idx : oldest_idx;
    memset(&s_devices[target], 0, sizeof(ble_device_t));
    s_devices[target].in_use = true;
    memcpy(s_devices[target].addr, addr, 6);
    s_devices[target].rssi = rssi;
    s_devices[target].last_seen_us = now;
    s_devices[target].last_reported_us = 0;
    strncpy(s_devices[target].name, name, MAX_NAME_LEN);
    s_devices[target].name[MAX_NAME_LEN] = '\0';
    s_devices[target].has_company_id = has_cid;
    s_devices[target].company_id = cid;
    s_devices[target].has_service_uuid16 = has_uuid16;
    s_devices[target].service_uuid16 = uuid16;

    xSemaphoreGive(s_dev_mutex);
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg) {
    if (event->type == BLE_GAP_EVENT_DISC) {
        struct ble_hs_adv_fields fields;
        char name[MAX_NAME_LEN + 1] = {0};
        bool has_cid = false;
        uint16_t cid = 0;
        bool has_uuid16 = false;
        uint16_t uuid16 = 0;

        if (ble_hs_adv_parse_fields(&fields,
                                    event->disc.data,
                                    event->disc.length_data) == 0) {
            if (fields.name != NULL && fields.name_len > 0) {
                sanitize_name(name, fields.name, fields.name_len);
            }
            if (fields.mfg_data != NULL && fields.mfg_data_len >= 2) {
                cid = (uint16_t)fields.mfg_data[0] | ((uint16_t)fields.mfg_data[1] << 8);
                has_cid = true;
            }
            if (fields.num_uuids16 > 0 && fields.uuids16 != NULL) {
                uuid16 = fields.uuids16[0].value;
                has_uuid16 = true;
            } else if (fields.svc_data_uuid16 != NULL && fields.svc_data_uuid16_len >= 2) {
                // Capture 16-bit Service Data UUIDs (used by Tile, SmartTag, Find My, etc.)
                uuid16 = (uint16_t)fields.svc_data_uuid16[0] |
                         ((uint16_t)fields.svc_data_uuid16[1] << 8);
                has_uuid16 = true;
            }
        }

        upsert_device(event->disc.addr.val, event->disc.rssi, name,
                      has_cid, cid, has_uuid16, uuid16);
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        start_ble_scan();
    }
    return 0;
}

static void start_ble_scan(void) {
    struct ble_gap_disc_params params = {0};
    params.filter_duplicates = 0;      // Keep RSSI and metadata updated
    params.passive           = 1;      // 1 = strictly passive; 0 = request scan responses
    params.itvl              = 0x0050; // 50 ms interval
    params.window            = 0x0050; // 50 ms window (100% duty cycle)
    params.filter_policy     = BLE_HCI_SCAN_FILT_NO_WL;
    params.limited           = 0;

    ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, ble_gap_event_cb, NULL);
}

static void ble_on_sync(void) {
    ble_hs_util_ensure_addr(0);
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) == 0) {
        start_ble_scan();
    }
}

static void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* Periodically streams newly seen / updated BLE devices over UART */
static void report_task(void *param) {
    // Announce OUI-Spy Detector mode at boot for immediate recognition
    vTaskDelay(pdMS_TO_TICKS(500));
    uart_send_line("{\"firmware\":\"OUI-SPY\",\"mode\":\"ble_detector\"}\n");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));
        int64_t now = esp_timer_get_time();

        for (int i = 0; i < MAX_DEVICES; i++) {
            ble_device_t snap;
            bool should_emit = false;

            if (xSemaphoreTake(s_dev_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (s_devices[i].in_use) {
                    if ((now - s_devices[i].last_seen_us) > STALE_TIMEOUT_US) {
                        s_devices[i].in_use = false;
                    } else if (s_devices[i].last_seen_us > s_devices[i].last_reported_us &&
                               (s_devices[i].last_reported_us == 0 ||
                                (now - s_devices[i].last_reported_us) >= REPORT_INTERVAL_US)) {
                        snap = s_devices[i];
                        s_devices[i].last_reported_us = now;
                        should_emit = true;
                    }
                }
                xSemaphoreGive(s_dev_mutex);
            }

            if (should_emit) {
                emit_device_json(&snap);
            }
        }
    }
}

static void dump_live_devices(void) {
    int64_t now = esp_timer_get_time();

    for (int i = 0; i < MAX_DEVICES; i++) {
        ble_device_t snap;
        bool valid = false;

        if (xSemaphoreTake(s_dev_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_devices[i].in_use && (now - s_devices[i].last_seen_us) <= STALE_TIMEOUT_US) {
                snap = s_devices[i];
                s_devices[i].last_reported_us = now;
                valid = true;
            }
            xSemaphoreGive(s_dev_mutex);
        }

        if (valid) {
            emit_device_json(&snap);
        }
    }
}

static void handle_command(char *cmd) {
    while (*cmd && isspace((unsigned char)*cmd)) cmd++;
    size_t len = strlen(cmd);
    while (len > 0 && isspace((unsigned char)cmd[len - 1])) {
        cmd[--len] = '\0';
    }
    if (len == 0) return;

    if (strcasecmp(cmd, "CMD:VERSION") == 0) {
        uart_send_line("{\"firmware\":\"OUI-SPY\",\"mode\":\"ble_detector\"}\n");
    } else if (strcasecmp(cmd, "CMD:DUMP_LIVE") == 0) {
        dump_live_devices();
    }
}

static void uart_cli_task(void *param) {
    uint8_t byte;
    char cmd_buf[128];
    size_t pos = 0;

    while (1) {
        int n = uart_read_bytes(UART_PORT, &byte, 1, pdMS_TO_TICKS(100));
        if (n <= 0) continue;

        if (byte == '\r' || byte == '\n') {
            if (pos > 0) {
                cmd_buf[pos] = '\0';
                handle_command(cmd_buf);
                pos = 0;
            }
        } else if (pos < sizeof(cmd_buf) - 1) {
            cmd_buf[pos++] = (char)byte;
        }
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    s_dev_mutex  = xSemaphoreCreateMutex();
    s_uart_mutex = xSemaphoreCreateMutex();

    uart_config_t uart_cfg = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_RX_BUF_SIZE, 0, 0, NULL, 0));

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);

    xTaskCreate(report_task, "ble_report", 4096, NULL, 4, NULL);
    xTaskCreate(uart_cli_task, "uart_cli", 4096, NULL, 5, NULL);
}