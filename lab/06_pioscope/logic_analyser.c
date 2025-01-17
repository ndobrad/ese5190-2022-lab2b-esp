/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// PIO logic analyser example
//
// This program captures samples from a group of pins, at a fixed rate, once a
// trigger condition is detected (level condition on one pin). The samples are
// transferred to a capture buffer using the system DMA.
//
// 1 to 32 pins can be captured, at a sample rate no greater than system clock
// frequency.

#include <stdio.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/structs/bus_ctrl.h"

#include "neopixel.h"

// Some logic to analyse:
const uint CAPTURE_PIN_BASE = 22;
const uint CAPTURE_PIN_COUNT = 2;
const uint CAPTURE_N_SAMPLES = 65535;
const uint BOOT_PIN = 21;

static inline uint bits_packed_per_word(uint pin_count) {
    // If the number of pins to be sampled divides the shift register size, we
    // can use the full SR and FIFO width, and push when the input shift count
    // exactly reaches 32. If not, we have to push earlier, so we use the FIFO
    // a little less efficiently.
    const uint SHIFT_REG_WIDTH = 32;
    return SHIFT_REG_WIDTH - (SHIFT_REG_WIDTH % pin_count);
}

void logic_analyser_init(PIO pio, uint sm, uint pin_base, uint pin_count, float div) {
    // Load a program to capture n pins. This is just a single `in pins, n`
    // instruction with a wrap.
    uint16_t capture_prog_instr = pio_encode_in(pio_pins, pin_count);
    struct pio_program capture_prog = {
            .instructions = &capture_prog_instr,
            .length = 1,
            .origin = -1
    };
    uint offset = pio_add_program(pio, &capture_prog);

    // Configure state machine to loop over this `in` instruction forever,
    // with autopush enabled.
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_in_pins(&c, pin_base);
    sm_config_set_wrap(&c, offset, offset);
    sm_config_set_clkdiv(&c, div);
    // Note that we may push at a < 32 bit threshold if pin_count does not
    // divide 32. We are using shift-to-right, so the sample data ends up
    // left-justified in the FIFO in this case, with some zeroes at the LSBs.
    sm_config_set_in_shift(&c, true, true, bits_packed_per_word(pin_count));
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    pio_sm_init(pio, sm, offset, &c);
}

void logic_analyser_arm(PIO pio, uint sm, uint dma_chan, uint32_t *capture_buf, size_t capture_size_words,
                        uint trigger_pin, bool trigger_level) {
    pio_sm_set_enabled(pio, sm, false);
    // Need to clear _input shift counter_, as well as FIFO, because there may be
    // partial ISR contents left over from a previous run. sm_restart does this.
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);

    dma_channel_config c = dma_channel_get_default_config(dma_chan);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, false));

    dma_channel_configure(dma_chan, &c,
        capture_buf,        // Destination pointer
        &pio->rxf[sm],      // Source pointer
        capture_size_words, // Number of transfers
        true                // Start immediately
    );

    pio_sm_exec(pio, sm, pio_encode_wait_gpio(trigger_level, trigger_pin));
    pio_sm_set_enabled(pio, sm, true);
}

void print_capture_buf(const uint32_t *buf, uint pin_base, uint pin_count, uint32_t n_samples) {
    // Display the capture buffer in text form, one sample per line, pins comma separated
    // Each FIFO record may be only partially filled with bits, depending on
    // whether pin_count is a factor of 32.
    uint record_size_bits = bits_packed_per_word(pin_count);
    putchar('U');
    printf("%02d,%02d\n", pin_base, pin_base + 1);
    for (int sample = 0; sample < n_samples; ++sample) {
        uint bit_index0 = sample * pin_count;
        uint word_index0 = bit_index0 / record_size_bits;
        uint bit_index1 = 1 + sample * pin_count;
        uint word_index1 = bit_index1 / record_size_bits;
        // Data is left-justified in each FIFO entry, hence the (32 - record_size_bits) offset
        uint word_mask0 = 1u << (bit_index0 % record_size_bits + 32 - record_size_bits);
        uint word_mask1 = 1u << (bit_index1 % record_size_bits + 32 - record_size_bits);
        printf(buf[word_index0] & word_mask0 ? "1," : "0,");
        printf(buf[word_index1] & word_mask1 ? "1" : "0");
        printf("\n");
    }
    printf(";\n");
    
}

int main() {
    stdio_init_all();
    //printf("PIO logic analyser example\n");
    gpio_init(BOOT_PIN);
    gpio_set_dir(BOOT_PIN, GPIO_IN);

    gpio_init(CAPTURE_PIN_BASE);
    gpio_init(CAPTURE_PIN_BASE + 1);
    gpio_set_dir(CAPTURE_PIN_BASE, GPIO_IN);
    gpio_set_dir(CAPTURE_PIN_BASE + 1, GPIO_IN);
    gpio_set_pulls(CAPTURE_PIN_BASE, true, false);
    gpio_set_pulls(CAPTURE_PIN_BASE + 1, true, false);

    neopixel_init();

    bool last_boot_pin_state = true; //store previous state to detect falling edge
    bool capture_active = false;
    bool dma_was_active = false;

   
    while (!stdio_usb_connected());
    //printf("Startup\n");
    // We're going to capture into a u32 buffer, for best DMA efficiency. Need
    // to be careful of rounding in case the number of pins being sampled
    // isn't a power of 2.
    uint total_sample_bits = CAPTURE_N_SAMPLES * CAPTURE_PIN_COUNT;
    total_sample_bits += bits_packed_per_word(CAPTURE_PIN_COUNT) - 1;
    uint buf_size_words = total_sample_bits / bits_packed_per_word(CAPTURE_PIN_COUNT);
    uint32_t *capture_buf = malloc(buf_size_words * sizeof(uint32_t));
    hard_assert(capture_buf);

    // Grant high bus priority to the DMA, so it can shove the processors out
    // of the way. This should only be needed if you are pushing things up to
    // >16bits/clk here, i.e. if you need to saturate the bus completely.
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    PIO pio = pio1;
    uint sm = 0;
    uint dma_chan = 0;

    logic_analyser_init(pio, sm, CAPTURE_PIN_BASE, CAPTURE_PIN_COUNT, 1.f);

    // The logic analyser should have started capturing as soon as it saw the
    // first transition. Wait until the last sample comes in from the DMA.
    //printf("Loop start\n");
    while(1) {
        bool pin_state = gpio_get(BOOT_PIN);
        if(!pin_state && last_boot_pin_state) {
            //printf("Arming trigger\n");
            logic_analyser_arm(pio, sm, dma_chan, capture_buf, buf_size_words, CAPTURE_PIN_BASE, false);
            capture_active = true;
            dma_was_active = false;
            //printf("trigger set\n");
            neopixel_set_rgb(0x50);
            
        }
        if (capture_active) {
            if (dma_was_active && !dma_channel_is_busy(dma_chan)) {
                capture_active = false;
                neopixel_set_rgb(0x4050);
                print_capture_buf(capture_buf, CAPTURE_PIN_BASE, CAPTURE_PIN_COUNT, CAPTURE_N_SAMPLES);
                neopixel_set_rgb(0x0);

            }
            if (dma_channel_is_busy(dma_chan)) { //don't know when there'll be activity on the input pins, so need to wait for DMA channel to be busy then not busy
                dma_was_active = true;
                neopixel_set_rgb(0x005000);
            }
        }

        last_boot_pin_state = pin_state;

    }

}
