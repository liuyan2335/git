/**
 * fb_lcd.h - RK3568 Framebuffer LCD Display Driver
 *
 * Provides low-level framebuffer operations for LCD display on RK3568.
 * Supports RGB565 pixel format, BMP image rendering, and screen refresh.
 *
 * Hardware: RK3568 + LCD panel via /dev/fb0
 */
#ifndef FB_LCD_H
#define FB_LCD_H

#include <linux/fb.h>
#include <stdint.h>

/* Default screen dimensions for typical RK3568 LCD */
#define LCD_WIDTH  1024
#define LCD_HEIGHT 600

/* Color depth: RGB565 (16-bit) */
#define COLOR_DEPTH 16

/* Display area partitions for multi-region UI */
#define UI_AREA_TOP      0
#define UI_AREA_MAIN     1
#define UI_AREA_BOTTOM   2
#define UI_MAX_AREAS     4

/* RGB565 color macros */
#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

/* Common UI colors in RGB565 */
#define COLOR_BLACK        0x0000
#define COLOR_WHITE        0xFFFF
#define COLOR_RED          0xF800
#define COLOR_GREEN        0x07E0
#define COLOR_BLUE         0x001F
#define COLOR_GRAY         0x8410
#define COLOR_LIGHT_GRAY   0xC618
#define COLOR_DARK_CYAN    0x07FF

/* UI area descriptor */
typedef struct {
    int x;
    int y;
    int width;
    int height;
    uint16_t *buffer;        /* pixel buffer for this area */
    int dirty;               /* 1 = needs refresh */
} ui_area_t;

/* BMP file header (14 bytes) */
#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;         /* 'BM' magic */
    uint32_t bfSize;         /* file size */
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;      /* offset to pixel data */
} bmp_file_header_t;

/* BMP info header (40 bytes for BITMAPINFOHEADER) */
typedef struct {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} bmp_info_header_t;
#pragma pack(pop)

/* Global framebuffer state */
typedef struct {
    int fd;                     /* /dev/fb0 file descriptor */
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    uint16_t *fbmem;            /* mmap'd framebuffer */
    size_t screensize;
    int screen_w;
    int screen_h;
    ui_area_t areas[UI_MAX_AREAS];
    int area_count;
} fb_state_t;

/* ----- API functions ----- */

/**
 * Initialize framebuffer: open /dev/fb0, get screen info, mmap memory.
 * Returns 0 on success, -1 on error (prints to stderr).
 */
int fb_init(fb_state_t *state);

/**
 * Release framebuffer resources: munmap, close fd.
 */
void fb_release(fb_state_t *state);

/**
 * Fill the entire screen with a single RGB565 color.
 * Use macros like COLOR_BLACK, RGB565(r,g,b), etc.
 */
void fb_fill_screen(fb_state_t *state, uint16_t color);

/**
 * Fill a rectangular region with a single color.
 * Coordinates are zero-based, clamped to screen bounds.
 */
void fb_fill_rect(fb_state_t *state, int x, int y, int w, int h, uint16_t color);

/**
 * Draw a single pixel at (x, y). No-op if out of bounds.
 */
void fb_draw_pixel(fb_state_t *state, int x, int y, uint16_t color);

/**
 * Draw a horizontal line from (x, y) for len pixels.
 */
void fb_draw_hline(fb_state_t *state, int x, int y, int len, uint16_t color);

/**
 * Draw a vertical line from (x, y) for len pixels.
 */
void fb_draw_vline(fb_state_t *state, int x, int y, int len, uint16_t color);

/**
 * Draw a hollow rectangle border (1px thick).
 */
void fb_draw_rect(fb_state_t *state, int x, int y, int w, int h, uint16_t color);

/**
 * Render a 24-bit BMP file to the framebuffer at position (dst_x, dst_y).
 * Converts 24-bit RGB to RGB565 on the fly.
 * Returns 0 on success, -1 on error.
 */
int fb_draw_bmp(fb_state_t *state, const char *bmp_path, int dst_x, int dst_y);

/**
 * Register a UI area for managed rendering.
 * Use to divide the screen into panels (top bar, main content, bottom bar).
 */
void fb_register_area(fb_state_t *state, int area_id, int x, int y, int w, int h);

/**
 * Refresh (flush) a specific area to the physical framebuffer.
 */
void fb_refresh_area(fb_state_t *state, int area_id);

/**
 * Refresh all dirty areas.
 */
void fb_refresh_all(fb_state_t *state);

/**
 * Clear an area to a solid color and mark dirty.
 */
void fb_clear_area(fb_state_t *state, int area_id, uint16_t color);

#endif /* FB_LCD_H */
