/*
    i2s_ioexpander.cpp
    Part of Grbl_ESP32

    Basic GPIO expander using the ESP32 I2S peripheral (I2S0 only)

    2020    - Michiyasu Odaki

    Grbl_ESP32 is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Grbl is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Grbl_ESP32.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2020 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <FreeRTOS.h>
#include <driver/periph_ctrl.h>
#include <rom/lldesc.h>
#include <soc/i2s_struct.h>
#include <freertos/queue.h>

#include "config.h"

#ifdef USE_I2S_IOEXPANDER

#include "i2s_ioexpander.h"

//
// define some Marlin macros
//
#ifndef SET_BIT_TO
#define SET_BIT_TO(N,B,TF) \
                        do{\
                            if (TF) {\
                                (N) |= (1UL << (B));\
                            } else {\
                                (N) &= ~(1UL << (B));\
                            }\
                        }while(0)
#endif
#ifndef TEST
#define TEST(n,b) (!!((n) & (1UL << (b))))
#endif

//
// configrations for DMA connected I2S
//
#define DMA_BUF_COUNT 8                                /* number of DMA buffers to store data */
#define DMA_BUF_LEN   4092                             /* maximum size in bytes (4092 is DMA's limit) */
#define I2S_SAMPLE_SIZE 4                              /* 4 bytes, 32 bits per sample */
#define DMA_SAMPLE_COUNT DMA_BUF_LEN / I2S_SAMPLE_SIZE /* number of samples per buffer */

typedef struct {
  uint32_t     **buffers;
  uint32_t     *current;
  uint32_t     rw_pos;
  lldesc_t     **desc;
  xQueueHandle queue;
} i2s_dma_t;

static portMUX_TYPE i2s_spinlock = portMUX_INITIALIZER_UNLOCKED;
static i2s_dma_t dma;

// output value
static volatile uint32_t i2s_port_data = 0;

#define I2S_ENTER_CRITICAL()  portENTER_CRITICAL(&i2s_spinlock)
#define I2S_EXIT_CRITICAL()   portEXIT_CRITICAL(&i2s_spinlock)

//
// External funtions (without init function)
//
void i2s_ioexpander_write(uint8_t pin, uint8_t val) {
  SET_BIT_TO(i2s_port_data, pin, val);
}

uint8_t i2s_ioexpander_state(uint8_t pin) {
  return TEST(i2s_port_data, pin);
}

uint32_t i2s_ioexpander_push_sample() {
  if (dma.rw_pos < DMA_SAMPLE_COUNT) {
    dma.current[dma.rw_pos++] = i2s_port_data;
    return 1;
  }
  return 0;
}

//
// Internal functions (and init function)
//
static inline void gpio_matrix_out_check(uint32_t gpio, uint32_t signal_idx, bool out_inv, bool oen_inv) {
  //if pin = -1, do not need to configure
  if (gpio != -1) {
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[gpio], PIN_FUNC_GPIO);
    gpio_set_direction((gpio_num_t)gpio, (gpio_mode_t)GPIO_MODE_DEF_OUTPUT);
    gpio_matrix_out(gpio, signal_idx, out_inv, oen_inv);
  }
}

static inline void i2s_reset_fifo_without_lock() {
  I2S0.conf.rx_fifo_reset = 1;
  I2S0.conf.rx_fifo_reset = 0;
  I2S0.conf.tx_fifo_reset = 1;
  I2S0.conf.tx_fifo_reset = 0;
}

static void i2s_reset_fifo() {
  I2S_ENTER_CRITICAL();
  i2s_reset_fifo_without_lock();
  I2S_EXIT_CRITICAL();
}

static int i2s_start() {
  //start DMA link
  I2S_ENTER_CRITICAL();
  i2s_reset_fifo_without_lock();

  //reset dma
  I2S0.lc_conf.in_rst = 1;
  I2S0.lc_conf.in_rst = 0;
  I2S0.lc_conf.out_rst = 1;
  I2S0.lc_conf.out_rst = 0;

  I2S0.conf.tx_reset = 1;
  I2S0.conf.tx_reset = 0;
  I2S0.conf.rx_reset = 1;
  I2S0.conf.rx_reset = 0;

  I2S0.int_clr.val = 0xFFFFFFFF;
  I2S0.out_link.start = 1;
  I2S0.conf.tx_start = 1;
  I2S_EXIT_CRITICAL();

  return 0;
}

static int i2s_stop() {
  I2S_ENTER_CRITICAL();
  I2S0.out_link.stop = 1;
  I2S0.conf.tx_start = 0;

  I2S0.int_clr.val = I2S0.int_st.val; //clear pending interrupt
  I2S_EXIT_CRITICAL();

  return 0;
}

//
// I2S DMA Interrupts handler
//
static void IRAM_ATTR i2s_intr_handler_default(void *arg) {
  int dummy;
  lldesc_t *finish_desc;
  portBASE_TYPE high_priority_task_awoken = pdFALSE;

  if (I2S0.int_st.out_eof) {
    // Get the descriptor of the last item in the linkedlist
    finish_desc = (lldesc_t*) I2S0.out_eof_des_addr;

    // If the queue is full it's because we have an underflow,
    // more than buf_count isr without new data, remove the front buffer
    if (xQueueIsQueueFullFromISR(dma.queue)) {
      xQueueReceiveFromISR(dma.queue, &dummy, &high_priority_task_awoken);
    }
    // Send a DMA complete event to the I2S bitstreamer task with finished buffer 
    xQueueSendFromISR(dma.queue, (void *)(&finish_desc->buf), &high_priority_task_awoken);
  }

  if (high_priority_task_awoken == pdTRUE) portYIELD_FROM_ISR();

  // clear interrupt
  I2S0.int_clr.val = I2S0.int_st.val; //clear pending interrupt
}

//
// I2S bitstream generator task
//
static void i2sIOExpanderTask(void* parameter) {
  i2s_ioexpander_init_t *stepper_param_p = (i2s_ioexpander_init_t*)parameter;
  while (1) {
    // Wait a DMA complete event from I2S isr
    // (Block until a DMA transfer has complete)
    xQueueReceive(dma.queue, &dma.current, portMAX_DELAY);
    // It reuses the oldest (just transferred) buffer with the name "current"
    // and fills the buffer for later DMA.
    dma.rw_pos = 0;
    while (dma.rw_pos < DMA_SAMPLE_COUNT) {
      // Fill with the extended GPIO port data
      stepper_param_p->pulse_phase_func();
      stepper_param_p->block_phase_func();
    }
  }
}

//
// Initialize funtion (external function)
//
int i2s_ioexpander_init(i2s_ioexpander_init_t &init_param) {
  periph_module_enable(PERIPH_I2S0_MODULE);

  /**
   * Each i2s transfer will take
   *   fpll = PLL_D2_CLK      -- clka_en = 0
   *
   *   fi2s = fpll / N + b/a  -- N = clkm_div_num
   *   fi2s = 160MHz / 2
   *   fi2s = 80MHz
   *
   *   fbclk = fi2s / M   -- M = tx_bck_div_num
   *   fbclk = 80MHz / 2
   *   fbclk = 40MHz
   *
   *   fwclk = fbclk / 32
   *
   *   for fwclk = 250kHz (16-bit: 4µS pulse time, 32-bit: 8μS pulse time)
   *      N = 10
   *      M = 2
   *   for fwclk = 500kHz (16-bit: 2µS pulse time, 32-bit: 4μS pulse time)
   *      N = 5
   *      M = 2
   */
  i2s_ioexpander_init_t *stepper_param_p = (i2s_ioexpander_init_t*)malloc(sizeof(i2s_ioexpander_init_t));
  if (stepper_param_p == nullptr) return -1;
  *stepper_param_p = init_param; // copy the struct members

  // Allocate the array of pointers to the buffers
  dma.buffers = (uint32_t **)malloc(sizeof(uint32_t*) * DMA_BUF_COUNT);
  if (dma.buffers == nullptr) return -1;

  // Allocate each buffer that can be used by the DMA controller
  for (int buf_idx = 0; buf_idx < DMA_BUF_COUNT; buf_idx++) {
    dma.buffers[buf_idx] = (uint32_t*) heap_caps_calloc(1, DMA_BUF_LEN, MALLOC_CAP_DMA);
    if (dma.buffers[buf_idx] == nullptr) return -1;
  }

  // Allocate the array of DMA descriptors
  dma.desc = (lldesc_t**) malloc(sizeof(lldesc_t*) * DMA_BUF_COUNT);
  if (dma.desc == nullptr) return -1;

  // Allocate each DMA descriptor that will be used by the DMA controller
  for (int buf_idx = 0; buf_idx < DMA_BUF_COUNT; buf_idx++) {
    dma.desc[buf_idx] = (lldesc_t*) heap_caps_malloc(sizeof(lldesc_t), MALLOC_CAP_DMA);
    if (dma.desc[buf_idx] == nullptr) return -1;
  }

  // Initialize
  for (int buf_idx = 0; buf_idx < DMA_BUF_COUNT; buf_idx++) {
    dma.desc[buf_idx]->owner = 1;
    dma.desc[buf_idx]->eof = 1; // set to 1 will trigger the interrupt
    dma.desc[buf_idx]->sosf = 0;
    dma.desc[buf_idx]->length = DMA_BUF_LEN;
    dma.desc[buf_idx]->size = DMA_BUF_LEN;
    dma.desc[buf_idx]->buf = (uint8_t *) dma.buffers[buf_idx];
    dma.desc[buf_idx]->offset = 0;
    dma.desc[buf_idx]->empty = (uint32_t)((buf_idx < (DMA_BUF_COUNT - 1)) ? (dma.desc[buf_idx + 1]) : dma.desc[0]);
  }

  dma.queue = xQueueCreate(DMA_BUF_COUNT, sizeof(uint32_t *));

   // Set the first DMA descriptor
  I2S0.out_link.addr = (uint32_t)dma.desc[0];

  // stop i2s
  i2s_stop();

  // configure I2S data port interface.
  i2s_reset_fifo();

  //reset i2s
  I2S0.conf.tx_reset = 1;
  I2S0.conf.tx_reset = 0;
  I2S0.conf.rx_reset = 1;
  I2S0.conf.rx_reset = 0;

  //reset dma
  I2S0.lc_conf.in_rst = 1;
  I2S0.lc_conf.in_rst = 0;
  I2S0.lc_conf.out_rst = 1;
  I2S0.lc_conf.out_rst = 0;

  //Enable and configure DMA
  I2S0.lc_conf.check_owner = 0;
  I2S0.lc_conf.out_loop_test = 0;
  I2S0.lc_conf.out_auto_wrback = 0;
  I2S0.lc_conf.out_data_burst_en = 0;
  I2S0.lc_conf.outdscr_burst_en = 0;
  I2S0.lc_conf.out_no_restart_clr = 0;
  I2S0.lc_conf.indscr_burst_en = 0;
  I2S0.lc_conf.out_eof_mode = 1;

  I2S0.conf2.lcd_en = 0;
  I2S0.conf2.camera_en = 0;
  I2S0.pdm_conf.pcm2pdm_conv_en = 0;
  I2S0.pdm_conf.pdm2pcm_conv_en = 0;

  I2S0.fifo_conf.dscr_en = 0;

  I2S0.conf_chan.tx_chan_mod = 0; // 0: Dual channel data mode
  I2S0.fifo_conf.tx_fifo_mod = 2; // 0: 16-bit dual channel data, 2: 32-bit dual channel data
  I2S0.conf.tx_mono = 0; // Set this bit to enable transmitter’s mono mode in PCM standard mode.

  I2S0.conf_chan.rx_chan_mod = 0; // 0: Dual channel data mode
  I2S0.fifo_conf.rx_fifo_mod = 2; // 0: 16-bit dual channel data, 2: 32-bit dual channel data
  I2S0.conf.rx_mono = 0;

  I2S0.fifo_conf.dscr_en = 1; //connect DMA to fifo

  I2S0.conf.tx_start = 0;
  I2S0.conf.rx_start = 0;

  I2S0.conf.tx_msb_right = 1; // Set this bit to place right-channel data at the MSB in the transmit FIFO.
  I2S0.conf.tx_right_first = 1; // Set this bit to transmit right-channel data first.

  I2S0.conf.tx_slave_mod = 0; // Master
  I2S0.fifo_conf.tx_fifo_mod_force_en = 1; //The bit should always be set to 1.

  I2S0.pdm_conf.rx_pdm_en = 0; // Set this bit to enable receiver’s PDM mode.
  I2S0.pdm_conf.tx_pdm_en = 0; // Set this bit to enable transmitter’s PDM mode.

  I2S0.conf.tx_short_sync = 0; // Set this bit to enable transmitter in PCM standard mode.
  I2S0.conf.rx_short_sync = 0; // Set this bit to enable receiver in PCM standard mode.
  I2S0.conf.tx_msb_shift = 0; // Do not use the Philips standard to avoid bit-shifting
  I2S0.conf.rx_msb_shift = 0; // Do not use the Philips standard to avoid bit-shifting

  // set clock (fi2s)
  I2S0.clkm_conf.clka_en = 0;       // Use 160 MHz PLL_D2_CLK as reference
  I2S0.clkm_conf.clkm_div_num = 5; // minimum value of 2, reset value of 4, max 256 (I²S clock divider’s integral value)
  I2S0.clkm_conf.clkm_div_a = 0;    // 0 at reset, what about divide by 0? (not an issue)
  I2S0.clkm_conf.clkm_div_b = 0;    // 0 at reset

  // Bit clock configuration bit in transmitter mode.
  // fbck = fi2s / tx_bck_div_num = (160 MHz / 5) / 2 = 16 MHz
  I2S0.sample_rate_conf.tx_bck_div_num = 2; // minimum value of 2 defaults to 6
  I2S0.sample_rate_conf.rx_bck_div_num = 2;
  I2S0.sample_rate_conf.tx_bits_mod = 32;
  I2S0.sample_rate_conf.rx_bits_mod = 32;

  // Enable TX interrupts (DMA Interrupts)
  I2S0.int_ena.out_eof = 1; // Triggered when rxlink has finished sending a packet.
  I2S0.int_ena.out_dscr_err = 0; // Triggered when invalid rxlink descriptors are encountered.
  I2S0.int_ena.out_total_eof = 0; // Triggered when all transmitting linked lists are used up.
  I2S0.int_ena.out_done = 0; // Triggered when all transmitted and buffered data have been read.

  // Allocate and Enable the I2S interrupt
  intr_handle_t i2s_isr_handle;
  esp_intr_alloc(ETS_I2S0_INTR_SOURCE, 0, i2s_intr_handler_default, nullptr, &i2s_isr_handle);
  esp_intr_enable(i2s_isr_handle);

  // Route the i2s pins to the appropriate GPIO
  gpio_matrix_out_check(init_param.data_pin, I2S0O_DATA_OUT23_IDX, 0, 0);
  gpio_matrix_out_check(init_param.bck_pin, I2S0O_BCK_OUT_IDX, 0, 0);
  gpio_matrix_out_check(init_param.ws_pin, I2S0O_WS_OUT_IDX, 0, 0);

  // Create the task that will feed the buffer
  xTaskCreatePinnedToCore(i2sIOExpanderTask,
                          "I2SIOExpanderTask",
                          1024 * 64,
                          stepper_param_p,
                          1,
                          nullptr,
                          CONFIG_ARDUINO_RUNNING_CORE  // must run the task on same core
                          );

  // Start the I2S peripheral
  return i2s_start();
}
#endif
