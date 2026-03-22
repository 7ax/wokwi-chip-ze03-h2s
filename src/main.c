#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZE03_FRAME_SIZE   9
#define ZE03_GAS_TYPE     0x04
#define ZE03_UNIT_BYTE    0x03
#define ACTIVE_INTERVAL   1000000  /* 1 second in microseconds */

typedef struct {
    uart_dev_t uart;
    uint32_t   h2s_attr;
    timer_t    active_timer;
    bool       active_mode;
    uint8_t    rx_buf[ZE03_FRAME_SIZE];
    uint8_t    rx_pos;
    bool       tx_busy;
    uint8_t    tx_buf[ZE03_FRAME_SIZE];
    uint32_t   fault_attr;
    uint32_t   warmup_attr;
    uint32_t   warmup_remaining;
} chip_state_t;

static uint8_t ze03_checksum(const uint8_t *frame) {
    uint8_t sum = 0;
    for (int i = 1; i <= 7; i++) {
        sum += frame[i];
    }
    return (uint8_t)(~sum + 1);
}

static uint16_t read_concentration(chip_state_t *state) {
    float ppm = attr_read_float(state->h2s_attr);
    if (ppm != ppm) ppm = 0.0f;  /* NaN guard: avoid UB in cast */
    if (ppm < 0.0f) ppm = 0.0f;
    if (ppm > 100.0f) ppm = 100.0f;
    return (uint16_t)(ppm * 100.0f + 0.5f);
}

static void send_active_frame(chip_state_t *state) {
    uint16_t raw;
    uint32_t fault = attr_read(state->fault_attr);

    if (fault == 1) return;  /* FAULT_NO_RESPONSE */

    if (state->tx_busy) return;

    raw = read_concentration(state);

    state->tx_buf[0] = 0xFF;
    state->tx_buf[1] = ZE03_GAS_TYPE;
    state->tx_buf[2] = ZE03_UNIT_BYTE;
    state->tx_buf[3] = (uint8_t)(raw >> 8);
    state->tx_buf[4] = (uint8_t)(raw & 0xFF);
    state->tx_buf[5] = 0x00;
    state->tx_buf[6] = 0x00;
    state->tx_buf[7] = 0x00;
    state->tx_buf[8] = ze03_checksum(state->tx_buf);

    if (fault == 2) state->tx_buf[8] ^= 0xFF;  /* FAULT_BAD_CHECKSUM */

    state->tx_busy = true;
    if (!uart_write(state->uart, state->tx_buf, ZE03_FRAME_SIZE)) {
        state->tx_busy = false;
        printf("[ZE03-H2S] uart_write failed\n");
    }
}

static void send_qa_response(chip_state_t *state) {
    uint16_t raw;
    uint32_t fault = attr_read(state->fault_attr);

    if (fault == 1) return;  /* FAULT_NO_RESPONSE */

    if (state->tx_busy) return;

    raw = read_concentration(state);

    state->tx_buf[0] = 0xFF;
    state->tx_buf[1] = 0x86;
    state->tx_buf[2] = (uint8_t)(raw >> 8);
    state->tx_buf[3] = (uint8_t)(raw & 0xFF);
    state->tx_buf[4] = 0x00;
    state->tx_buf[5] = 0x00;
    state->tx_buf[6] = 0x00;
    state->tx_buf[7] = 0x00;
    state->tx_buf[8] = ze03_checksum(state->tx_buf);

    if (fault == 2) state->tx_buf[8] ^= 0xFF;  /* FAULT_BAD_CHECKSUM */

    state->tx_busy = true;
    if (!uart_write(state->uart, state->tx_buf, ZE03_FRAME_SIZE)) {
        state->tx_busy = false;
        printf("[ZE03-H2S] uart_write failed\n");
    }
}

static void on_timer(void *user_data) {
    chip_state_t *state = (chip_state_t *)user_data;

    if (state->warmup_remaining > 0) {
        state->warmup_remaining--;
        if (state->warmup_remaining == 0) {
            printf("[ZE03-H2S] Warm-up complete\n");
        }
        return;
    }

    if (state->active_mode) {
        send_active_frame(state);
    }
}

static void on_write_done(void *user_data) {
    chip_state_t *state = (chip_state_t *)user_data;
    state->tx_busy = false;
}

static void process_command(chip_state_t *state) {
    uint8_t *buf = state->rx_buf;

    if (state->warmup_remaining > 0) return;

    if (buf[0] != 0xFF) return;
    if (buf[8] != ze03_checksum(buf)) {
        printf("[ZE03-H2S] Invalid checksum\n");
        return;
    }

    /* Q&A read: FF 86 00 00 00 00 00 00 7A */
    if (buf[1] == 0x86) {
        send_qa_response(state);
        return;
    }

    if (buf[1] == 0x01) {
        /* Q&A read (datasheet format): FF 01 86 00 00 00 00 00 79 */
        if (buf[2] == 0x86) {
            send_qa_response(state);
            return;
        }
        /* Mode switch: FF 01 78 MODE 00 00 00 00 CS */
        if (buf[2] == 0x78) {
            if (buf[3] == 0x41 && state->active_mode) {
                state->active_mode = false;
                timer_stop(state->active_timer);
                printf("[ZE03-H2S] Switched to Q&A mode\n");
            } else if (buf[3] == 0x40 && !state->active_mode) {
                state->active_mode = true;
                timer_start(state->active_timer, ACTIVE_INTERVAL, true);
                printf("[ZE03-H2S] Switched to active upload mode\n");
            }
        }
    }
}

/*
 * Frame resync note: after a checksum failure, rx_pos resets to 0 and any
 * partially-consumed valid frame bytes are discarded.  This is inherent to
 * the ZE03's single-byte (0xFF) sync protocol and matches real hardware
 * behavior — adding a sliding-window resync would introduce more bug
 * surface than it eliminates.
 */
static void on_rx_data(void *user_data, uint8_t byte) {
    chip_state_t *state = (chip_state_t *)user_data;

    if (state->rx_pos == 0 && byte != 0xFF) {
        return;
    }

    state->rx_buf[state->rx_pos] = byte;
    state->rx_pos++;

    if (state->rx_pos >= ZE03_FRAME_SIZE) {
        process_command(state);
        state->rx_pos = 0;
    }
}

void chip_init(void) {
    chip_state_t *state = malloc(sizeof(chip_state_t));
    if (!state) {
        printf("[ZE03-H2S] Failed to allocate state\n");
        return;
    }
    memset(state, 0, sizeof(chip_state_t));

    state->active_mode = true;
    state->h2s_attr = attr_init_float("h2s_ppm", 2.0f);
    state->fault_attr = attr_init("fault_mode", 0);
    state->warmup_attr = attr_init("warmup_ticks", 0);
    state->warmup_remaining = attr_read(state->warmup_attr);

    pin_t tx_pin = pin_init("TX", OUTPUT);
    pin_t rx_pin = pin_init("RX", INPUT);

    const uart_config_t uart_cfg = {
        .user_data = state,
        .tx = tx_pin,
        .rx = rx_pin,
        .baud_rate = 9600,
        .rx_data = on_rx_data,
        .write_done = on_write_done,
        .reserved = {0},
    };
    state->uart = uart_init(&uart_cfg);

    const timer_config_t timer_cfg = {
        .user_data = state,
        .callback = on_timer,
        .reserved = {0},
    };
    state->active_timer = timer_init(&timer_cfg);
    timer_start(state->active_timer, ACTIVE_INTERVAL, true);

    printf("[ZE03-H2S] Initialized (active upload mode, default 2.00 ppm, warmup=%lu)\n",
           (unsigned long)state->warmup_remaining);
}
