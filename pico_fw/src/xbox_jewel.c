// Copyright (c) 2024 Raspberry Pi (Trading) Ltd.

// Generate DVI output using the command expander and TMDS encoder in HSTX.

// This example requires an external digital video connector connected to
// GPIOs 12 through 19 (the HSTX-capable GPIOs) with appropriate
// current-limiting resistors, e.g. 270 ohms. The pinout used in this example
// matches the Pico DVI Sock board, which can be soldered onto a Pico 2:
// https://github.com/Wren6991/Pico-DVI-Sock

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/regs/dma.h"

#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"

#include "hardware/structs/io_bank0.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "pico/platform/compiler.h"
#include "pico/time.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// #include "images/animation.h"
// #include "images/flat_green_proto.h"
// #include "images/flat_green_proto_still.h"
// #include "images/flat_green_retail.h"
// #include "images/powered_by_directx.h"
// #include "images/343.h"
#include "images/animation.h"


// ----------------------------------------------------------------------------
// DVI constants

#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

// ----------------------------------------------------------------------------
// HDMI constants

#define TERC4_0000 0b1010011100
#define TERC4_0001 0b1001100011
#define TERC4_0010 0b1011100100
#define TERC4_0011 0b1011100010
#define TERC4_0100 0b0101110001
#define TERC4_0101 0b0100011110
#define TERC4_0110 0b0110001110
#define TERC4_0111 0b0100111100
#define TERC4_1000 0b1011001100
#define TERC4_1001 0b0100111001
#define TERC4_1010 0b0110011100
#define TERC4_1011 0b1011000110
#define TERC4_1100 0b1010001110
#define TERC4_1101 0b1001110001
#define TERC4_1110 0b0101100011
#define TERC4_1111 0b1011000011

#define VIDEO_GB_PREAMBLE (TMDS_CTRL_11 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_00 << 20))
#define VIDEO_GB          (0b1011001100 | (0b0100110011 << 10) | (0b1011001100 << 20))

#define DATA_GB_PREAMBLE (TMDS_CTRL_11 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))
#define DATA_GB          (TERC4_1111 | (0b0100110011 << 10) | (0b0100110011 << 20))

// ----------------------------------------------------------------------------
// Display timing constants

#define MODE_H_SYNC_POLARITY 0
#define MODE_H_FRONT_PORCH   68
#define MODE_H_SYNC_WIDTH    32
#define MODE_H_BACK_PORCH    200
#define MODE_H_ACTIVE_PIXELS 800

#define MODE_V_SYNC_POLARITY 0
#define MODE_V_FRONT_PORCH   16
#define MODE_V_SYNC_WIDTH    4
#define MODE_V_BACK_PORCH    80
#define MODE_V_ACTIVE_LINES  800

#define TMDS_DATA_RATE_KHz 594000

#define MODE_H_TOTAL_PIXELS ( \
    MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH + \
    MODE_H_BACK_PORCH  + MODE_H_ACTIVE_PIXELS \
)
#define MODE_V_TOTAL_LINES  ( \
    MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + \
    MODE_V_BACK_PORCH  + MODE_V_ACTIVE_LINES \
)

// ----------------------------------------------------------------------------
// HSTX constants

#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

// ----------------------------------------------------------------------------
// HSTX command lists

static const uint32_t vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V1_H1,
};

static const uint32_t vblank_line_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
    SYNC_V0_H1,
};

static const uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT  | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT  | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT  | (MODE_H_BACK_PORCH - 10),
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT  | 8,
    VIDEO_GB_PREAMBLE,
    HSTX_CMD_RAW_REPEAT  | 2,
    VIDEO_GB,
};

// Enhanced DVI DMA channels
#define DMACH_PING 0
#define DMACH_PONG 1

// Channel for fetching lines from flash
#define DMACH_FLASH 2

static uint8_t __attribute__((aligned(4))) frame_buf[2][256000];
volatile int frame_buf_sel = 0;

static uintptr_t s_next_dma_buf;

static const uint16_t (*line_sizes)[800];

static const uint16_t* s_line_size;
static const uint16_t* next_line_size;

/*
   Volatile here is bad practice.
   However, core1 reads/writes multiple times within the same function, and optimizations interefere.
   Better synchronization methods take up more time and thus cause issues in dma_irq_handler() below.
   There is an effective race condition between core0 reading this and core1 writing it.
   We avoid these issues by being careful about how we interact with these values on both cores.
*/
static const uint16_t* volatile current_line_size;

// A ping and a pong are cued up initially, so the first time we enter the irq
// handler it is to cue up the second ping after the first ping and pong have completed.
// This is the third scanline overall (-> 2 due to zero-indexing).
volatile uint v_scanline = 2;

// This function is very time critical, and therefore very sensitive to changes.
void __scratch_x("") dma_irq_handler() {
    // First we ping. Then we pong. Then... we ping again.
    static bool dma_pong = false;
    // dma_pong indicates the channel that just finished, which is the one
    // we're about to reload.
    uint ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    dma_pong = !dma_pong;

    // During the vertical active period, we take two IRQs per scanline: one to
    // post the command list, and another to post the pixels.
    static bool vactive_cmdlist_posted = false;

    if (v_scanline >= MODE_V_FRONT_PORCH && v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH)) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = count_of(vblank_line_vsync_on);
        s_next_dma_buf = (uintptr_t)&frame_buf[frame_buf_sel];
        s_line_size = current_line_size;
    } else if (v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + (MODE_V_BACK_PORCH)) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = count_of(vblank_line_vsync_off);
    } else if (!vactive_cmdlist_posted) {
        ch->read_addr = (uintptr_t)vactive_line;
        ch->transfer_count = count_of(vactive_line);
        vactive_cmdlist_posted = true;
    } else {
        ch->read_addr = s_next_dma_buf;
        ch->transfer_count = *s_line_size / sizeof(uint32_t);
        vactive_cmdlist_posted = false;
        s_next_dma_buf += *s_line_size++;
    }

    if (!vactive_cmdlist_posted) {
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
    }
}

static const uint8_t** frame_list;       // List of pointers to framedata for each frame
static const uint32_t* frame_sizes_list; // List of frame sizes
static uintptr_t next_frame;             // Tracks next pointer to framedata
static unsigned int next_frame_size;     // Tracks next frame's size (variable due to compression)
static unsigned int total_frames;        // Total frames in the animation

static inline void dma_data(void) {
    dma_hw->ch[DMACH_FLASH].read_addr = next_frame;
    dma_hw->ch[DMACH_FLASH].write_addr = (uintptr_t)&frame_buf[frame_buf_sel^1];
    dma_hw->ch[DMACH_FLASH].transfer_count = next_frame_size / sizeof(uint32_t);
    dma_hw->ch[DMACH_FLASH].ctrl_trig |= DMA_CH0_CTRL_TRIG_EN_BITS; // Start the DMA.
}

void __not_in_flash_func(core1_dma_framebuffer)(void) {
    /* Tracks number of frames displayed on the screen.
       Used to control framerate of the animation, via a frame divisor.*/
    static unsigned int frame_count = 0;
    static unsigned int frames_displayed = 2;

    // DMA the initial frame into the unfilled framebuffer
    dma_data();

    next_frame = (uintptr_t)frame_list[frames_displayed % total_frames];
    next_frame_size = frame_sizes_list[frames_displayed % total_frames];

    while(1) {
        // When new frames start, we need to increment the frame counter
        if(!v_scanline) {
            frame_count = (frame_count + 1) % 4;

            // Once frame count wraps to 0, we're on the hook to fill the next framebuffer quickly enough
            if(!frame_count) {
                // Swap the frame buffer to the one we just filled
                frame_buf_sel ^= 1;

                // Swap the list to the one for the frame we're about to display
                current_line_size = next_line_size;

                // Copy the new frame into SRAM
                dma_data();

                // Get the next set of line sizes ready.
                next_line_size = &line_sizes[frames_displayed][0];

                // Increment the displayed frame counter
                frames_displayed++;

                // Reset to 0 once we reach total_frames
                if(frames_displayed == total_frames) {
                    frames_displayed = 0;
                }

                // Prepare the next frame DMA address and DMA size
                next_frame = (uintptr_t)frame_list[frames_displayed];
                next_frame_size = frame_sizes_list[frames_displayed];
            }

            // Wait for the scanline to update. We have a bit of CPU time on this core that we need to waste to make sure everything stays in sync.
            while(!v_scanline);
        }
    }
}

/*
    Run Length Encoded Video.
    Specific to the rp2350 HSTX hardware, video data comes pre-compressed and with HSTX commands pre-encoded.
    Video data is losslessly compressed, and comes with all the data we need for copying each line (which are variable length) correctly.
*/

typedef struct {
    char magic[4];
    uint32_t frame_count;
    uint32_t dma_sizes_offset;
    uint32_t frame_sizes_offset;
    uint32_t frame_data_offset;
} rlev_header;

int parse_rlev(const uint8_t* rlev) {
    const rlev_header* header = (const rlev_header*)rlev;

    if(memcmp(header->magic, "RLEV", 4)) {
        return -1;
    }

    total_frames = header->frame_count;

    /*
       Use the USB DPRAM for the frame list, as we aren't using USB here
       This helps avoid dynamic allocations. There's plenty of space for what we need.
       This gives us more space in the main SRAM for framebuffers.
    */
    frame_list = (const uint8_t**)0x50100000;
    unsigned int frame_offset = 0;
    frame_sizes_list = (const uint32_t*)(&rlev[header->frame_sizes_offset]);
    for(int i = 0; i < total_frames; i++) {
        // Let the DMA hardware bypass the XIP cache for speed;
        // we're always looking at something "new" from the cache's perspective, so it only adds latency.
        frame_list[i] = &rlev[header->frame_data_offset + frame_offset] + 0x04000000;
        frame_offset += frame_sizes_list[i];
    }

    // Bypass the XIP cache, for speed
    line_sizes = (const uint16_t (*)[800])(&rlev[header->dma_sizes_offset] + 0x04000000);

    if(total_frames < 2) {
        // Set up initial parameters
        next_line_size = &line_sizes[0][0];
        next_frame = (uintptr_t)frame_list[0];
        next_frame_size = frame_sizes_list[0];
        current_line_size = &line_sizes[0][0];
    } else {
        // Set up initial parameters
        next_line_size = &line_sizes[1][0];
        next_frame = (uintptr_t)frame_list[1];
        next_frame_size = frame_sizes_list[1];
        current_line_size = &line_sizes[0][0];
    }

    return 0;
}

void hstx_init(void) {
    // Configure HSTX's TMDS encoder for RGB332
    hstx_ctrl_hw->expand_tmds =
    2  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
    0  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
    2  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
    29 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
    1  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
    26 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    // Pixels (TMDS) come in 4 8-bit chunks. Control symbols (RAW) are an
    // entire 32-bit word.
    hstx_ctrl_hw->expand_shift =
        4  << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        8  << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB    |
        1  << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0  << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    // Serial output config: clock period of 5 cycles, pop from command
    // expander every 5 cycles, shift the output shiftreg by 2 every cycle, for 10 total bits.
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // HSTX outputs 0 through 7 appear on GPIO 12 through 19.
    // Pinout on Pico DVI sock:
    //
    //   GP12 D0+  GP13 D0-
    //   GP14 CK+  GP15 CK-
    //   GP16 D2+  GP17 D2-
    //   GP18 D1+  GP19 D1-

    // Assign clock pair to two neighbouring pins:
    hstx_ctrl_hw->bit[2] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[3] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    for (uint lane = 0; lane < 3; ++lane) {
        // For each TMDS lane, assign it to the correct GPIO pair based on the
        // desired pinout:
        static const int lane_to_output_bit[3] = {0, 6, 4};
        int bit = lane_to_output_bit[lane];
        // Output even bits during first half of each HSTX cycle, and odd bits
        // during second half. The shifter advances by two bits each cycle.
        uint32_t lane_data_sel_bits =
            (lane * 10    ) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        // The two halves of each pair get identical data, but one pin is inverted.
        hstx_ctrl_hw->bit[bit    ] = lane_data_sel_bits;
        hstx_ctrl_hw->bit[bit + 1] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
    }

    for (int i = 12; i <= 19; ++i) {
        gpio_set_function(i, GPIO_FUNC_HSTX);
    }
}

int main(void) {
    // Set the system clock to half of the TMDS data rate (HSTX is DDR)
    if(set_sys_clock_khz(TMDS_DATA_RATE_KHz/2, false) == false) {
        while(1) {
            // printf("Couldn't set clock!\n");
            reset_usb_boot(0, 0);
            sleep_ms(5000);
        }
    }

    hstx_init();

    parse_rlev(animation);

    // Both channels are set up identically, to transfer a whole scanline and
    // then chain to the opposite channel. Each time a channel finishes, we
    // reconfigure the one that just finished, meanwhile the opposite channel
    // is already making progress.
    dma_channel_config c;
    c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(
        DMACH_PING,
        &c,
        &hstx_fifo_hw->fifo,
        vblank_line_vsync_off,
        count_of(vblank_line_vsync_off),
        false
    );
    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(
        DMACH_PONG,
        &c,
        &hstx_fifo_hw->fifo,
        vblank_line_vsync_off,
        count_of(vblank_line_vsync_off),
        false
    );

    // Configure the DMA channel that will DMA frames from flash to SRAM
    c = dma_channel_get_default_config(DMACH_FLASH);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);

    // Get the first frame into SRAM, so we have something to send to HSTX immediately
    dma_channel_configure(
        DMACH_FLASH,
        &c,
        &frame_buf[0],
        frame_list[0],
        frame_sizes_list[0] / sizeof(uint32_t),
        true
    );
    while(dma_channel_is_busy(DMACH_FLASH));

    // Enable interrupts for both channels
    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    // The other core handles filling the framebuffers from now on.
    multicore_launch_core1(core1_dma_framebuffer);

    // Start the first DMA transfer
    dma_channel_start(DMACH_PING);

    while(1) {
        __wfi();
    }
}
