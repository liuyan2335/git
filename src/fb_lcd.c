/**
 * fb_lcd.c - RK3568 Framebuffer LCD Display Implementation
 *
 * Direct framebuffer manipulation via /dev/fb0.
 * Uses mmap() for zero-copy pixel writes, supports RGB565 color format,
 * BMP 24-bit image decoding and rendering.
 *
 * Compile: aarch64-linux-gcc -o fb_lcd fb_lcd.c -static
 * Usage:   ./fb_lcd [bmp_file]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <time.h>

#include "fb_lcd.h"

/* ================================================================
 *  Framebuffer Initialization
 * ================================================================ */

int fb_init(fb_state_t *state)
{
    if (!state) return -1;

    memset(state, 0, sizeof(fb_state_t));

    /* Open the framebuffer device */
    state->fd = open("/dev/fb0", O_RDWR);
    if (state->fd < 0) {
        fprintf(stderr, "[fb_lcd] ERROR: Cannot open /dev/fb0: %s\n", strerror(errno));
        fprintf(stderr, "[fb_lcd] HINT: Try 'sudo chmod 666 /dev/fb0' or run as root.\n");
        return -1;
    }

    /* Get fixed screen information */
    if (ioctl(state->fd, FBIOGET_FSCREENINFO, &state->finfo) < 0) {
        fprintf(stderr, "[fb_lcd] ERROR: FBIOGET_FSCREENINFO failed: %s\n", strerror(errno));
        close(state->fd);
        return -1;
    }

    /* Get variable screen information */
    if (ioctl(state->fd, FBIOGET_VSCREENINFO, &state->vinfo) < 0) {
        fprintf(stderr, "[fb_lcd] ERROR: FBIOGET_VSCREENINFO failed: %s\n", strerror(errno));
        close(state->fd);
        return -1;
    }

    /* Map framebuffer memory */
    state->screensize = state->finfo.smem_len;
    state->fbmem = (uint16_t *)mmap(0, state->screensize,
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED, state->fd, 0);

    if (state->fbmem == MAP_FAILED) {
        fprintf(stderr, "[fb_lcd] ERROR: mmap failed: %s\n", strerror(errno));
        close(state->fd);
        return -1;
    }

    state->screen_w = state->vinfo.xres;
    state->screen_h = state->vinfo.yres;

    printf("[fb_lcd] Framebuffer initialized: %dx%d, %d bpp, %zu bytes mapped\n",
           state->screen_w, state->screen_h,
           state->vinfo.bits_per_pixel, state->screensize);
    printf("[fb_lcd]   Line length: %d, Pixel format: RGB565 (R:%d G:%d B:%d)\n",
           state->finfo.line_length,
           state->vinfo.red.length, state->vinfo.green.length, state->vinfo.blue.length);

    state->area_count = 0;
    return 0;
}

/* ================================================================
 *  Framebuffer Cleanup
 * ================================================================ */

void fb_release(fb_state_t *state)
{
    if (!state) return;

    /* Free area buffers */
    for (int i = 0; i < state->area_count; i++) {
        if (state->areas[i].buffer) {
            free(state->areas[i].buffer);
            state->areas[i].buffer = NULL;
        }
    }

    if (state->fbmem && state->fbmem != MAP_FAILED) {
        munmap(state->fbmem, state->screensize);
    }
    if (state->fd >= 0) {
        close(state->fd);
    }
    printf("[fb_lcd] Framebuffer released.\n");
}

/* ================================================================
 *  Basic Drawing Primitives
 * ================================================================ */

void fb_draw_pixel(fb_state_t *state, int x, int y, uint16_t color)
{
    if (!state || !state->fbmem) return;
    if (x < 0 || x >= state->screen_w || y < 0 || y >= state->screen_h) return;

    long offset = y * state->finfo.line_length / 2 + x;  /* /2 because uint16_t* */
    state->fbmem[offset] = color;
}

void fb_fill_screen(fb_state_t *state, uint16_t color)
{
    if (!state || !state->fbmem) return;

    int total_pixels = state->screen_w * state->screen_h;
    for (int i = 0; i < total_pixels; i++) {
        state->fbmem[i] = color;
    }
}

void fb_fill_rect(fb_state_t *state, int x, int y, int w, int h, uint16_t color)
{
    if (!state || !state->fbmem) return;

    /* Clamp to screen bounds */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > state->screen_w) w = state->screen_w - x;
    if (y + h > state->screen_h) h = state->screen_h - y;
    if (w <= 0 || h <= 0) return;

    int stride = state->finfo.line_length / 2;
    for (int row = 0; row < h; row++) {
        long offset = (y + row) * stride + x;
        for (int col = 0; col < w; col++) {
            state->fbmem[offset + col] = color;
        }
    }
}

void fb_draw_hline(fb_state_t *state, int x, int y, int len, uint16_t color)
{
    fb_fill_rect(state, x, y, len, 1, color);
}

void fb_draw_vline(fb_state_t *state, int x, int y, int len, uint16_t color)
{
    fb_fill_rect(state, x, y, 1, len, color);
}

void fb_draw_rect(fb_state_t *state, int x, int y, int w, int h, uint16_t color)
{
    fb_draw_hline(state, x, y, w, color);               /* top */
    fb_draw_hline(state, x, y + h - 1, w, color);       /* bottom */
    fb_draw_vline(state, x, y, h, color);                /* left */
    fb_draw_vline(state, x + w - 1, y, h, color);        /* right */
}

/* ================================================================
 *  BMP Image Rendering
 * ================================================================ */

int fb_draw_bmp(fb_state_t *state, const char *bmp_path, int dst_x, int dst_y)
{
    if (!state || !state->fbmem || !bmp_path) return -1;

    FILE *fp = fopen(bmp_path, "rb");
    if (!fp) {
        fprintf(stderr, "[fb_lcd] ERROR: Cannot open BMP file '%s': %s\n",
                bmp_path, strerror(errno));
        return -1;
    }

    bmp_file_header_t file_hdr;
    bmp_info_header_t info_hdr;

    /* Read file header */
    if (fread(&file_hdr, sizeof(file_hdr), 1, fp) != 1) {
        fprintf(stderr, "[fb_lcd] ERROR: Failed to read BMP file header.\n");
        fclose(fp);
        return -1;
    }

    /* Verify BMP signature */
    if (file_hdr.bfType != 0x4D42) {  /* 'BM' in little-endian */
        fprintf(stderr, "[fb_lcd] ERROR: Not a valid BMP file (bad magic: 0x%04X).\n",
                file_hdr.bfType);
        fclose(fp);
        return -1;
    }

    /* Read info header */
    if (fread(&info_hdr, sizeof(info_hdr), 1, fp) != 1) {
        fprintf(stderr, "[fb_lcd] ERROR: Failed to read BMP info header.\n");
        fclose(fp);
        return -1;
    }

    /* Only support 24-bit uncompressed BMP */
    if (info_hdr.biBitCount != 24 || info_hdr.biCompression != 0) {
        fprintf(stderr, "[fb_lcd] ERROR: Only 24-bit uncompressed BMP supported "
                "(got %d-bit, compression=%d).\n",
                info_hdr.biBitCount, info_hdr.biCompression);
        fclose(fp);
        return -1;
    }

    int img_w = info_hdr.biWidth;
    int img_h = abs(info_hdr.biHeight);  /* height might be negative (top-down) */
    int top_down = (info_hdr.biHeight < 0);

    /* BMP rows are aligned to 4 bytes */
    int row_padded = (img_w * 3 + 3) & ~3;
    uint8_t *row_buf = (uint8_t *)malloc(row_padded);
    if (!row_buf) {
        fprintf(stderr, "[fb_lcd] ERROR: malloc failed for BMP row buffer.\n");
        fclose(fp);
        return -1;
    }

    /* Seek to pixel data */
    fseek(fp, file_hdr.bfOffBits, SEEK_SET);

    /* Render row by row */
    for (int row = 0; row < img_h; row++) {
        if (fread(row_buf, row_padded, 1, fp) != 1) {
            fprintf(stderr, "[fb_lcd] ERROR: Truncated BMP file at row %d.\n", row);
            free(row_buf);
            fclose(fp);
            return -1;
        }

        /* BMP rows are stored bottom-to-top unless top_down */
        int screen_y = top_down ? (dst_y + row) : (dst_y + img_h - 1 - row);

        /* Skip rows outside screen */
        if (screen_y < 0 || screen_y >= state->screen_h) continue;

        for (int col = 0; col < img_w; col++) {
            int screen_x = dst_x + col;
            if (screen_x < 0 || screen_x >= state->screen_w) continue;

            /* BMP stores BGR, not RGB */
            uint8_t b = row_buf[col * 3];
            uint8_t g = row_buf[col * 3 + 1];
            uint8_t r = row_buf[col * 3 + 2];

            uint16_t rgb565 = RGB565(r, g, b);
            fb_draw_pixel(state, screen_x, screen_y, rgb565);
        }
    }

    free(row_buf);
    fclose(fp);

    printf("[fb_lcd] BMP rendered: %s (%dx%d) at (%d,%d)\n",
           bmp_path, img_w, img_h, dst_x, dst_y);
    return 0;
}

/* ================================================================
 *  UI Area Management
 * ================================================================ */

void fb_register_area(fb_state_t *state, int area_id, int x, int y, int w, int h)
{
    if (!state || area_id < 0 || area_id >= UI_MAX_AREAS) return;

    /* Clamp dimensions */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > state->screen_w) w = state->screen_w - x;
    if (y + h > state->screen_h) h = state->screen_h - y;
    if (w <= 0 || h <= 0) return;

    ui_area_t *area = &state->areas[area_id];
    area->x = x;
    area->y = y;
    area->width = w;
    area->height = h;

    /* Allocate or reallocate buffer */
    size_t buf_size = w * h * sizeof(uint16_t);
    uint16_t *new_buf = (uint16_t *)realloc(area->buffer, buf_size);
    if (new_buf) {
        area->buffer = new_buf;
        memset(area->buffer, 0, buf_size);
    }
    area->dirty = 1;

    if (area_id >= state->area_count) {
        state->area_count = area_id + 1;
    }
}

void fb_refresh_area(fb_state_t *state, int area_id)
{
    if (!state || area_id < 0 || area_id >= state->area_count) return;

    ui_area_t *area = &state->areas[area_id];
    if (!area->buffer) return;

    int stride = state->finfo.line_length / 2;
    for (int row = 0; row < area->height; row++) {
        long dst_offset = (area->y + row) * stride + area->x;
        long src_offset = row * area->width;
        memcpy(&state->fbmem[dst_offset], &area->buffer[src_offset],
               area->width * sizeof(uint16_t));
    }
    area->dirty = 0;
}

void fb_refresh_all(fb_state_t *state)
{
    if (!state) return;
    for (int i = 0; i < state->area_count; i++) {
        if (state->areas[i].dirty) {
            fb_refresh_area(state, i);
        }
    }
}

void fb_clear_area(fb_state_t *state, int area_id, uint16_t color)
{
    if (!state || area_id < 0 || area_id >= state->area_count) return;

    ui_area_t *area = &state->areas[area_id];
    if (!area->buffer) return;

    int total = area->width * area->height;
    for (int i = 0; i < total; i++) {
        area->buffer[i] = color;
    }
    area->dirty = 1;
}

/* ================================================================
 *  Standalone Demo: Fill screen, draw UI decorations, render BMP
 * ================================================================ */

#ifdef FB_LCD_STANDALONE

static void demo_ui(fb_state_t *fb, const char *bmp_path)
{
    printf("[demo] Running LCD display demo...\n");

    /* Full-screen black background */
    fb_fill_screen(fb, COLOR_BLACK);

    /* Top status bar area: dark background */
    fb_register_area(fb, UI_AREA_TOP, 0, 0, fb->screen_w, 40);
    fb_clear_area(fb, UI_AREA_TOP, COLOR_DARK_CYAN);

    /* Main content area */
    fb_register_area(fb, UI_AREA_MAIN, 0, 40,
                     fb->screen_w, fb->screen_h - 80);

    /* Bottom button bar */
    fb_register_area(fb, UI_AREA_BOTTOM, 0, fb->screen_h - 40,
                     fb->screen_w, 40);
    fb_clear_area(fb, UI_AREA_BOTTOM, COLOR_GRAY);

    /* Render BMP to main area if provided */
    if (bmp_path) {
        fb_clear_area(fb, UI_AREA_MAIN, COLOR_BLACK);
        fb_refresh_area(fb, UI_AREA_MAIN);
        fb_draw_bmp(fb, bmp_path, 100, 60);
    } else {
        /* Draw a simple pattern in the main area */
        fb_clear_area(fb, UI_AREA_MAIN, COLOR_BLACK);
        fb_refresh_area(fb, UI_AREA_MAIN);
        for (int i = 0; i < 10; i++) {
            int x = 50 + i * 90;
            fb_draw_rect(fb, x, 80, 80, 50, COLOR_GREEN);
        }
    }

    /* Refresh all areas */
    fb_refresh_all(fb);

    printf("[demo] Display updated. Press Enter to exit...\n");
    getchar();
}

int main(int argc, char *argv[])
{
    fb_state_t fb;

    printf("=== RK3568 Framebuffer LCD Demo ===\n");

    if (fb_init(&fb) < 0) {
        fprintf(stderr, "Failed to initialize framebuffer.\n");
        return 1;
    }

    const char *bmp_file = (argc > 1) ? argv[1] : NULL;
    demo_ui(&fb, bmp_file);

    fb_release(&fb);
    return 0;
}

#endif /* FB_LCD_STANDALONE */
