#include <stdint.h>
#include <stdio.h>

#define CHIPS_IMPL
#define COMMON_IMPL

#include "bitmap.h"
#include "chips/clk.h"
#include "chips/mem.h"
#include "chips/z80.h"
#include "clock.h"
#include "gfx.h"
#include "rygar-roms.h"
#include "sokol_app.h"
#include "sprite.h"
#include "tile.h"
#include "tilemap.h"

#define BETWEEN(n, a, b) ((n >= a) && (n <= b))

#define CHAR_ROM_SIZE 0x10000
#define FG_ROM_SIZE 0x40000
#define BG_ROM_SIZE 0x40000
#define SPRITE_ROM_SIZE 0x40000

#define CHAR_RAM_SIZE 0x800
#define CHAR_RAM_START 0xd000
#define CHAR_RAM_END (CHAR_RAM_START + CHAR_RAM_SIZE - 1)

#define FG_RAM_SIZE 0x400
#define FG_RAM_START 0xd800
#define FG_RAM_END (FG_RAM_START + FG_RAM_SIZE - 1)

#define BG_RAM_SIZE 0x400
#define BG_RAM_START 0xdc00
#define BG_RAM_END (BG_RAM_START + BG_RAM_SIZE - 1)

#define SPRITE_RAM_SIZE 0x800
#define SPRITE_RAM_START 0xe000

#define PALETTE_RAM_SIZE 0x800
#define PALETTE_RAM_START 0xe800
#define PALETTE_RAM_END (PALETTE_RAM_START + PALETTE_RAM_SIZE - 1)

#define WORK_RAM_SIZE 0x1000
#define WORK_RAM_START 0xc000

#define RAM_SIZE 0x3000
#define RAM_START 0xc000
#define RAM_END (RAM_START + RAM_SIZE - 1)

#define BANK_SIZE 0x8000
#define BANK_WINDOW_SIZE 0x800
#define BANK_WINDOW_START 0xf000
#define BANK_WINDOW_END (BANK_WINDOW_START + BANK_WINDOW_SIZE - 1)

/* inputs */
#define JOYSTICK1 0xf800
#define BUTTONS1 0xf801
#define JOYSTICK2 0xf802
#define BUTTONS2 0xf803
#define SYS1 0xf804
#define SYS2 0xf805
#define DIP_SW1_L 0xf806
#define DIP_SW1_H 0xf807
#define DIP_SW2_L 0xf808
#define DIP_SW2_H 0xf809
#define SYS3 0xf80f

/* outputs */
#define FG_SCROLL_START 0xf800
#define FG_SCROLL_END 0xf802
#define BG_SCROLL_START 0xf803
#define BG_SCROLL_END 0xf805
#define SOUND_LATCH 0xf807
#define FLIP_SCREEN 0xf807
#define BANK_SWITCH 0xf808

#define BUFFER_WIDTH 256
#define BUFFER_HEIGHT 256

#define SCREEN_WIDTH 256
#define SCREEN_HEIGHT 224

/* The tilemap horizontal scroll values are all offset by a fixed value,
 * probably because of hardware timing constraints, etc. We don't want to
 * include this offset in our scroll values, so we must correct it. */
#define SCROLL_OFFSET 48

#define CPU_FREQ 4000000
#define VSYNC_PERIOD_4MHZ (4000000 / 60)
#define VBLANK_DURATION_4MHZ (((4000000 / 60) / 525) * (525 - 483))

typedef struct {
  clk_t clk;
  z80_t cpu;
  mem_t mem;

  /* ram */
  uint8_t work_ram[WORK_RAM_SIZE];
  uint8_t char_ram[CHAR_RAM_SIZE];
  uint8_t fg_ram[FG_RAM_SIZE];
  uint8_t bg_ram[BG_RAM_SIZE];
  uint8_t sprite_ram[SPRITE_RAM_SIZE];
  uint8_t palette_ram[PALETTE_RAM_SIZE];

  /* bank switched rom */
  uint8_t banked_rom[BANK_SIZE];
  uint8_t current_bank;

  /* tile roms */
  uint8_t char_rom[CHAR_ROM_SIZE];
  uint8_t fg_rom[FG_ROM_SIZE];
  uint8_t bg_rom[BG_ROM_SIZE];
  uint8_t sprite_rom[SPRITE_ROM_SIZE];

  /* input registers */
  uint8_t joystick;
  uint8_t buttons;
  uint8_t sys;

  /* tilemap scroll offset registers */
  uint8_t fg_scroll[3];
  uint8_t bg_scroll[3];
} mainboard_t;

typedef struct {
  mainboard_t main;

  bitmap_t bitmap;

  /* tilemaps */
  tilemap_t char_tilemap;
  tilemap_t fg_tilemap;
  tilemap_t bg_tilemap;

  /* 32-bit RGBA color palette cache */
  uint32_t palette[1024];

  /* counters */
  int vsync_count;
  int vblank_count;
} rygar_t;

rygar_t rygar;

/**
 * Updates the color palette cache with 32-bit colors, this is called for CPU
 * writes to the palette RAM area.
 *
 * The hardware palette contains 1024 entries of 16-bit big-endian color values
 * (xxxxBBBBRRRRGGGG). This function keeps a palette cache with 32-bit colors
 * up to date, so that the 32-bit colors don't need to be computed for each
 * pixel in the video decoding code.
 */
static inline void rygar_update_palette(uint16_t addr, uint8_t data) {
  uint16_t pal_index = addr >> 1;
  uint32_t c = rygar.palette[pal_index];

  if (addr & 1) {
    /* odd addresses are the RRRRGGGG part */
    uint8_t r = (data & 0xf0) | ((data >> 4) & 0x0f);
    uint8_t g = (data & 0x0f) | ((data << 4) & 0xf0);
    c = 0xff000000 | (c & 0x00ff0000) | g << 8 | r;
  } else {
    /* even addresses are the xxxxBBBB part */
    uint8_t b = (data & 0x0f) | ((data << 4) & 0xf0);
    c = 0xff000000 | (c & 0x0000ffff) | b << 16;
  }

  rygar.palette[pal_index] = c;
}

/**
 * This callback function is called for every CPU tick.
 */
static uint64_t rygar_tick_main(int num_ticks, uint64_t pins, void *user_data) {
  rygar.vsync_count -= num_ticks;

  if (rygar.vsync_count <= 0) {
    rygar.vsync_count += VSYNC_PERIOD_4MHZ;
    rygar.vblank_count = VBLANK_DURATION_4MHZ;
  }

  if (rygar.vblank_count > 0) {
    rygar.vblank_count -= num_ticks;
    pins |= Z80_INT; /* activate INT pin during VBLANK */
  } else {
    rygar.vblank_count = 0;
  }

  uint16_t addr = Z80_GET_ADDR(pins);

  if (pins & Z80_MREQ) {
    if (pins & Z80_WR) {
      uint8_t data = Z80_GET_DATA(pins);

      if (BETWEEN(addr, RAM_START, RAM_END)) {
        mem_wr(&rygar.main.mem, addr, data);

        if (BETWEEN(addr, CHAR_RAM_START, CHAR_RAM_END)) {
          tilemap_mark_tile_dirty(&rygar.char_tilemap, (addr - CHAR_RAM_START) & 0x3ff);
        } else if (BETWEEN(addr, FG_RAM_START, FG_RAM_END)) {
          tilemap_mark_tile_dirty(&rygar.fg_tilemap, (addr - FG_RAM_START) & 0x1ff);
        } else if (BETWEEN(addr, BG_RAM_START, BG_RAM_END)) {
          tilemap_mark_tile_dirty(&rygar.bg_tilemap, (addr - BG_RAM_START) & 0x1ff);
        } else if (BETWEEN(addr, PALETTE_RAM_START, PALETTE_RAM_END)) {
          rygar_update_palette(addr - PALETTE_RAM_START, data);
        }
      } else if (BETWEEN(addr, FG_SCROLL_START, FG_SCROLL_END)) {
        uint8_t offset = addr - FG_SCROLL_START;
        rygar.main.fg_scroll[offset] = data;
        tilemap_set_scroll_x(&rygar.fg_tilemap, (rygar.main.fg_scroll[1] << 8 | rygar.main.fg_scroll[0]) + SCROLL_OFFSET);
      } else if (BETWEEN(addr, BG_SCROLL_START, BG_SCROLL_END)) {
        uint8_t offset = addr - BG_SCROLL_START;
        rygar.main.bg_scroll[offset] = data;
        tilemap_set_scroll_x(&rygar.bg_tilemap, (rygar.main.bg_scroll[1] << 8 | rygar.main.bg_scroll[0]) + SCROLL_OFFSET);
      } else if (addr == BANK_SWITCH) {
        rygar.main.current_bank = data >> 3; /* bank addressed by DO3-DO6 in schematic */
      }
    } else if (pins & Z80_RD) {
      if (addr <= RAM_END) {
        Z80_SET_DATA(pins, mem_rd(&rygar.main.mem, addr));
      } else if (BETWEEN(addr, BANK_WINDOW_START, BANK_WINDOW_END)) {
        uint16_t banked_addr = addr - BANK_WINDOW_START + (rygar.main.current_bank * BANK_WINDOW_SIZE);
        Z80_SET_DATA(pins, rygar.main.banked_rom[banked_addr]);
      } else if (addr == JOYSTICK1) {
        Z80_SET_DATA(pins, rygar.main.joystick);
      } else if (addr == BUTTONS1) {
        Z80_SET_DATA(pins, rygar.main.buttons);
      } else if (addr == SYS1) {
        Z80_SET_DATA(pins, rygar.main.sys);
      } else if (addr == DIP_SW2_H) {
        Z80_SET_DATA(pins, 0x8);
      } else {
        Z80_SET_DATA(pins, 0);
      }
    }
  } else if ((pins & Z80_IORQ) && (pins & Z80_M1)) {
    /* clear interrupt */
    pins &= ~Z80_INT;
  }

  return pins;
}

static void char_tile_info(tile_t *tile, int index) {
  uint8_t lo = rygar.main.char_ram[index];
  uint8_t hi = rygar.main.char_ram[index + 0x400];

  /* the tile code is a 10-bit value, represented by the low byte and the
   * two LSBs of the high byte */
  tile->code = (hi & 0x03) << 8 | lo;

  /* the four MSBs of the high byte represent the color value */
  tile->color = hi >> 4;
}

static void fg_tile_info(tile_t *tile, int index) {
  uint8_t lo = rygar.main.fg_ram[index];
  uint8_t hi = rygar.main.fg_ram[index + 0x200];

  /* the tile code is a 11-bit value, represented by the low byte and the three
   * LSBs of the high byte */
  tile->code = (hi & 0x07) << 8 | lo;

  /* the four MSBs of the high byte represent the color value */
  tile->color = hi >> 4;
}

static void bg_tile_info(tile_t *tile, int index) {
  uint8_t lo = rygar.main.bg_ram[index];
  uint8_t hi = rygar.main.bg_ram[index + 0x200];

  /* the tile code is a 11-bit value, represented by the low byte and the three
   * LSBs of the high byte */
  tile->code = (hi & 0x07) << 8 | lo;

  /* the four MSBs of the high byte represent the color value */
  tile->color = hi >> 4;
}

/**
 * Decodes the tile ROMs.
 */
static void rygar_decode_tiles() {
  uint8_t tmp[0x20000];

  /* decode descriptor for a 8x8 tile */
  tile_decode_desc_t tile_decode_8x8 = {
    .tile_width = 8,
    .tile_height = 8,
    .planes = 4,
    .plane_offsets = { STEP4(0, 1) },
    .x_offsets = { STEP8(0, 4) },
    .y_offsets = { STEP8(0, 4 * 8) },
    .tile_size = 4 * 8 * 8
  };

  /* decode descriptor for a 16x16 tile, made up of four 8x8 tiles */
  tile_decode_desc_t tile_decode_16x16 = {
    .tile_width = 16,
    .tile_height = 16,
    .planes = 4,
    .plane_offsets = { STEP4(0, 1) },
    .x_offsets = { STEP8(0, 4), STEP8(4 * 8 * 8, 4) },
    .y_offsets = { STEP8(0, 4 * 8), STEP8(4 * 8 * 8 * 2, 4 * 8) },
    .tile_size = 4 * 4 * 8 * 8
  };

  /* tx rom */
  memcpy(&tmp[0x00000], dump_cpu_8k, 0x8000);

  tile_decode(&tile_decode_8x8, (uint8_t *)&tmp, (uint8_t *)&rygar.main.char_rom, 1024);

  tilemap_init(&rygar.char_tilemap, &(tilemap_desc_t) {
    .tile_cb = char_tile_info,
    .rom = rygar.main.char_rom,
    .tile_width = 8,
    .tile_height = 8,
    .cols = 32,
    .rows = 32
  });

  /* fg rom */
  memcpy(&tmp[0x00000], dump_vid_6p, 0x8000);
  memcpy(&tmp[0x08000], dump_vid_6o, 0x8000);
  memcpy(&tmp[0x10000], dump_vid_6n, 0x8000);
  memcpy(&tmp[0x18000], dump_vid_6l, 0x8000);

  tile_decode(&tile_decode_16x16, (uint8_t *)&tmp, (uint8_t *)&rygar.main.fg_rom, 1024);

  tilemap_init(&rygar.fg_tilemap, &(tilemap_desc_t) {
    .tile_cb = fg_tile_info,
    .rom = rygar.main.fg_rom,
    .tile_width = 16,
    .tile_height = 16,
    .cols = 32,
    .rows = 16
  });

  /* bg rom */
  memcpy(&tmp[0x00000], dump_vid_6f, 0x8000);
  memcpy(&tmp[0x08000], dump_vid_6e, 0x8000);
  memcpy(&tmp[0x10000], dump_vid_6c, 0x8000);
  memcpy(&tmp[0x18000], dump_vid_6b, 0x8000);

  tile_decode(&tile_decode_16x16, (uint8_t *)&tmp, (uint8_t *)&rygar.main.bg_rom, 1024);

  tilemap_init(&rygar.bg_tilemap, &(tilemap_desc_t) {
    .tile_cb = bg_tile_info,
    .rom = rygar.main.bg_rom,
    .tile_width = 16,
    .tile_height = 16,
    .cols = 32,
    .rows = 16
  });

  /* sprite rom */
  memcpy(&tmp[0x00000], dump_vid_6k, 0x8000);
  memcpy(&tmp[0x08000], dump_vid_6j, 0x8000);
  memcpy(&tmp[0x10000], dump_vid_6h, 0x8000);
  memcpy(&tmp[0x18000], dump_vid_6g, 0x8000);

  tile_decode(&tile_decode_8x8, (uint8_t *)&tmp, (uint8_t *)&rygar.main.sprite_rom, 4096);
}

/**
 * Initialises the Rygar arcade hardware.
 */
static void rygar_init() {
  memset(&rygar, 0, sizeof(rygar));

  rygar.vsync_count = VSYNC_PERIOD_4MHZ;

  clk_init(&rygar.main.clk, CPU_FREQ);
  z80_init(&rygar.main.cpu, &(z80_desc_t) { .tick_cb = rygar_tick_main });
  mem_init(&rygar.main.mem);
  bitmap_init(&rygar.bitmap, BUFFER_WIDTH, BUFFER_HEIGHT);

  /* main memory */
  mem_map_rom(&rygar.main.mem, 0, 0x0000, 0x8000, dump_5);
  mem_map_rom(&rygar.main.mem, 0, 0x8000, 0x4000, dump_cpu_5m);
  mem_map_ram(&rygar.main.mem, 0, WORK_RAM_START, WORK_RAM_SIZE, rygar.main.work_ram);
  mem_map_ram(&rygar.main.mem, 0, CHAR_RAM_START, CHAR_RAM_SIZE, rygar.main.char_ram);
  mem_map_ram(&rygar.main.mem, 0, FG_RAM_START, FG_RAM_SIZE, rygar.main.fg_ram);
  mem_map_ram(&rygar.main.mem, 0, BG_RAM_START, BG_RAM_SIZE, rygar.main.bg_ram);
  mem_map_ram(&rygar.main.mem, 0, SPRITE_RAM_START, SPRITE_RAM_SIZE, rygar.main.sprite_ram);
  mem_map_ram(&rygar.main.mem, 0, PALETTE_RAM_START, PALETTE_RAM_SIZE, rygar.main.palette_ram);

  /* banked rom */
  memcpy(&rygar.main.banked_rom[0x00000], dump_cpu_5j, 0x8000);

  rygar_decode_tiles();
}

static void rygar_shutdown() {
  bitmap_shutdown(&rygar.bitmap);
  tilemap_shutdown(&rygar.char_tilemap);
  tilemap_shutdown(&rygar.fg_tilemap);
  tilemap_shutdown(&rygar.bg_tilemap);
}

/**
 * Draws the graphics layers to the frame buffer.
 */
static void rygar_draw() {
  bitmap_t *bitmap = &rygar.bitmap;

  /* clear frame buffer */
  uint32_t *buffer = gfx_framebuffer();
  memset(buffer, 0, BUFFER_WIDTH * BUFFER_HEIGHT * sizeof(uint32_t));

  /* fill bitmap with the background color */
  bitmap_fill(bitmap, 0x100);

  /* draw layers */
  tilemap_draw(&rygar.bg_tilemap, bitmap, 0x300, TILE_LAYER3);
  tilemap_draw(&rygar.fg_tilemap, bitmap, 0x200, TILE_LAYER2);
  tilemap_draw(&rygar.char_tilemap, bitmap, 0x100, TILE_LAYER1);
  sprite_draw(bitmap, (uint8_t *)&rygar.main.sprite_ram, (uint8_t *)&rygar.main.sprite_rom, 0, TILE_LAYER0);

  /* skip the first 16 lines */
  uint16_t *data = bitmap->data + (16 * bitmap->width);

  /* copy bitmap to 32-bit frame buffer */
  for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
    *buffer++ = rygar.palette[*data++];
  }
}

/**
 * Runs the emulation for one frame.
 */
static void rygar_exec(uint32_t delta) {
  uint32_t ticks_to_run = clk_ticks_to_run(&rygar.main.clk, delta);
  uint32_t ticks_executed = 0;

  while (ticks_executed < ticks_to_run) {
    ticks_executed += z80_exec(&rygar.main.cpu, ticks_to_run);
  }
  clk_ticks_executed(&rygar.main.clk, ticks_executed);

  rygar_draw();
}

static void app_init() {
  gfx_init(&(gfx_desc_t) {
    .aspect_x = 4,
    .aspect_y = 3
  });
  clock_init();
  rygar_init();
}

static void app_frame() {
  rygar_exec(clock_frame_time());
  gfx_draw(SCREEN_WIDTH, SCREEN_HEIGHT);
}

static void app_input(const sapp_event *event) {
  switch (event->type) {
    case SAPP_EVENTTYPE_KEY_DOWN:
      switch (event->key_code) {
        case SAPP_KEYCODE_LEFT:  rygar.main.joystick |= (1 << 0); break;
        case SAPP_KEYCODE_RIGHT: rygar.main.joystick |= (1 << 1); break;
        case SAPP_KEYCODE_DOWN:  rygar.main.joystick |= (1 << 2); break;
        case SAPP_KEYCODE_UP:    rygar.main.joystick |= (1 << 3); break;
        case SAPP_KEYCODE_Z:     rygar.main.buttons |= (1 << 0); break; /* attack */
        case SAPP_KEYCODE_X:     rygar.main.buttons |= (1 << 1); break; /* jump */
        case SAPP_KEYCODE_1:     rygar.main.sys |= (1 << 2); break; /* player 1 coin */
        default:                 rygar.main.sys |= (1 << 1); break; /* player 1 start */
      }
      break;

    case SAPP_EVENTTYPE_KEY_UP:
      switch (event->key_code) {
        case SAPP_KEYCODE_LEFT:  rygar.main.joystick &= ~(1 << 0); break;
        case SAPP_KEYCODE_RIGHT: rygar.main.joystick &= ~(1 << 1); break;
        case SAPP_KEYCODE_DOWN:  rygar.main.joystick &= ~(1 << 2); break;
        case SAPP_KEYCODE_UP:    rygar.main.joystick &= ~(1 << 3); break;
        case SAPP_KEYCODE_Z:     rygar.main.buttons &= ~(1 << 0); break; /* attack */
        case SAPP_KEYCODE_X:     rygar.main.buttons &= ~(1 << 1); break; /* jump */
        case SAPP_KEYCODE_1:     rygar.main.sys &= ~(1 << 2); break; /* player 1 coin */
        default:                 rygar.main.sys &= ~(1 << 1); break; /* player 1 start */
      }
      break;

    default:
      break;
  }
}

static void app_cleanup() {
  rygar_shutdown();
  gfx_shutdown();
}

sapp_desc sokol_main(int argc, char *argv[]) {
  return (sapp_desc) {
    .init_cb = app_init,
    .frame_cb = app_frame,
    .event_cb = app_input,
    .cleanup_cb = app_cleanup,
    .width = SCREEN_WIDTH * 4,
    .height = SCREEN_HEIGHT * 3,
    .window_title = "Rygar"
  };
}
