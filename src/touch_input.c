/**
 * touch_input.c - RK3568 Touchscreen Input Implementation
 *
 * Reads from /dev/input/event* to capture touch events.
 * Supports auto-detection of touch input device, coordinate mapping,
 * gesture recognition (tap, swipe, long-press).
 *
 * Compile: aarch64-linux-gcc -o touch_input touch_input.c -static
 * Usage:   ./touch_input [/dev/input/eventX]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <errno.h>

#include "touch_input.h"

/* ================================================================
 *  Time Helper (milliseconds since boot)
 * ================================================================ */

static uint64_t get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ================================================================
 *  Auto-detect Touch Input Device
 * ================================================================ */

static int touch_auto_detect(char *device_path, int path_size)
{
    DIR *dir;
    struct dirent *entry;
    char path[256];
    int found = 0;

    dir = opendir("/dev/input");
    if (!dir) {
        fprintf(stderr, "[touch] ERROR: Cannot open /dev/input directory.\n");
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        /* Only examine event* devices */
        if (strncmp(entry->d_name, "event", 5) != 0) continue;

        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        /* Check device capabilities */
        unsigned long abs_bits[ABS_CNT / (sizeof(unsigned long) * 8) + 1];
        memset(abs_bits, 0, sizeof(abs_bits));

        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) {
            close(fd);
            continue;
        }

        /* Check for ABS_MT_POSITION_X — multi-touch capable device */
        int has_mt_x = (abs_bits[ABS_MT_POSITION_X / (sizeof(unsigned long) * 8)]
                        >> (ABS_MT_POSITION_X % (sizeof(unsigned long) * 8))) & 1;

        if (has_mt_x) {
            /* Get device name */
            char dev_name[256] = "unknown";
            ioctl(fd, EVIOCGNAME(sizeof(dev_name)), dev_name);

            snprintf(device_path, path_size, "%s", path);
            printf("[touch] Auto-detected touch device: %s (%s)\n", path, dev_name);
            found = 1;
            close(fd);
            break;
        }

        close(fd);
    }

    closedir(dir);

    if (!found) {
        fprintf(stderr, "[touch] ERROR: No touch input device found.\n");
        fprintf(stderr, "[touch] HINT: Check 'ls /dev/input/event*' and "
                "'cat /proc/bus/input/devices'.\n");
        return -1;
    }

    return 0;
}

/* ================================================================
 *  Touch Initialization
 * ================================================================ */

int touch_init(touch_state_t *state, const char *device_path)
{
    if (!state) return -1;

    memset(state, 0, sizeof(touch_state_t));

    if (device_path) {
        strncpy(state->device_path, device_path, sizeof(state->device_path) - 1);
    } else {
        if (touch_auto_detect(state->device_path, sizeof(state->device_path)) < 0) {
            return -1;
        }
    }

    state->fd = open(state->device_path, O_RDONLY);
    if (state->fd < 0) {
        fprintf(stderr, "[touch] ERROR: Cannot open %s: %s\n",
                state->device_path, strerror(errno));
        return -1;
    }

    /* Get screen resolution from the touch device abs info */
    struct input_absinfo abs_x, abs_y;
    if (ioctl(state->fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_x) == 0) {
        state->screen_w = abs_x.maximum;
    } else {
        state->screen_w = 1024;  /* fallback */
    }
    if (ioctl(state->fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y) == 0) {
        state->screen_h = abs_y.maximum;
    } else {
        state->screen_h = 600;   /* fallback */
    }

    printf("[touch] Device opened: %s (resolution: %dx%d)\n",
           state->device_path, state->screen_w, state->screen_h);
    return 0;
}

/* ================================================================
 *  Touch Cleanup
 * ================================================================ */

void touch_release(touch_state_t *state)
{
    if (!state) return;
    if (state->fd >= 0) {
        close(state->fd);
        state->fd = -1;
    }
    printf("[touch] Device closed.\n");
}

/* ================================================================
 *  Raw Event Reading
 * ================================================================ */

int touch_read_raw(touch_state_t *state, struct input_event *ev)
{
    if (!state || !ev) return -1;

    ssize_t n = read(state->fd, ev, sizeof(struct input_event));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        fprintf(stderr, "[touch] Read error: %s\n", strerror(errno));
        return -1;
    }
    if (n == 0) return -1;  /* EOF */
    if (n != sizeof(struct input_event)) return -1;

    return 0;
}

/* ================================================================
 *  Poll and Process Touch Events
 * ================================================================ */

void touch_map_coords(touch_state_t *state, int raw_x, int raw_y,
                       int *screen_x, int *screen_y)
{
    /* Simple linear mapping — override if axis inversion is needed */
    *screen_x = raw_x;
    *screen_y = raw_y;
}

int touch_in_rect(int tx, int ty, int rx, int ry, int rw, int rh)
{
    return (tx >= rx && tx <= rx + rw && ty >= ry && ty <= ry + rh);
}

static int find_point_index(touch_state_t *state, int tracking_id)
{
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
        if (state->points[i].active && state->points[i].id == tracking_id) {
            return i;
        }
    }
    return -1;
}

static int alloc_point_index(touch_state_t *state)
{
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
        if (!state->points[i].active) {
            return i;
        }
    }
    return -1;  /* all slots occupied */
}

int touch_poll(touch_state_t *state, touch_gesture_t *gesture)
{
    if (!state || !gesture) return -1;

    memset(gesture, 0, sizeof(touch_gesture_t));
    gesture->type = TOUCH_EVENT_NONE;

    struct input_event ev;
    int events_processed = 0;

    while (1) {
        int ret = touch_read_raw(state, &ev);
        if (ret < 0) return -1;
        if (ret == 0) break;  /* no more events */
        events_processed++;

        switch (ev.type) {
        case EV_ABS:
            switch (ev.code) {
            case ABS_MT_SLOT:
                /* Some devices use slot-based tracking */
                break;

            case ABS_MT_TRACKING_ID: {
                if (ev.value >= 0) {
                    /* Touch down — allocate a new tracking point */
                    int idx = alloc_point_index(state);
                    if (idx >= 0) {
                        state->points[idx].id = ev.value;
                        state->points[idx].active = 1;
                        state->points[idx].start_time_ms = get_time_ms();
                        state->points[idx].last_time_ms = get_time_ms();
                        state->points[idx].start_x = state->points[idx].x;
                        state->points[idx].start_y = state->points[idx].y;

                        gesture->type = TOUCH_EVENT_DOWN;
                        gesture->x = state->points[idx].x;
                        gesture->y = state->points[idx].y;
                    }
                } else {
                    /* Touch up — find and release the point */
                    /* ev.value == -1 on touch release */
                    for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
                        if (state->points[i].active) {
                            touch_point_t *pt = &state->points[i];
                            uint64_t duration = get_time_ms() - pt->start_time_ms;
                            int dx = pt->x - pt->start_x;
                            int dy = pt->y - pt->start_y;
                            int dist = abs(dx) + abs(dy);

                            /* Detect gestures */
                            if (duration < TAP_MAX_TIME_MS && dist < TAP_MAX_DISTANCE) {
                                /* Check for double tap */
                                uint64_t since_last = get_time_ms() - state->last_tap_time_ms;
                                if (since_last < 400 &&
                                    abs(pt->x - state->last_tap_x) < 30 &&
                                    abs(pt->y - state->last_tap_y) < 30) {
                                    gesture->type = TOUCH_EVENT_DOUBLE_TAP;
                                } else {
                                    gesture->type = TOUCH_EVENT_TAP;
                                }
                                state->last_tap_time_ms = get_time_ms();
                                state->last_tap_x = pt->x;
                                state->last_tap_y = pt->y;
                            } else if (duration >= LONG_PRESS_TIME_MS && dist < TAP_MAX_DISTANCE) {
                                gesture->type = TOUCH_EVENT_LONG_PRESS;
                            } else if (dist >= SWIPE_MIN_DISTANCE) {
                                /* Determine swipe direction */
                                if (abs(dx) > abs(dy)) {
                                    gesture->type = (dx > 0) ?
                                        TOUCH_EVENT_SWIPE_RIGHT : TOUCH_EVENT_SWIPE_LEFT;
                                } else {
                                    gesture->type = (dy > 0) ?
                                        TOUCH_EVENT_SWIPE_DOWN : TOUCH_EVENT_SWIPE_UP;
                                }
                                gesture->dx = dx;
                                gesture->dy = dy;
                            }

                            gesture->x = pt->x;
                            gesture->y = pt->y;
                            pt->active = 0;
                            pt->id = -1;
                            break;
                        }
                    }
                }
                break;
            }

            case ABS_MT_POSITION_X: {
                /* Update X for the most recently active point */
                for (int i = MAX_TOUCH_POINTS - 1; i >= 0; i--) {
                    if (state->points[i].active) {
                        state->points[i].x = ev.value;
                        state->points[i].last_time_ms = get_time_ms();
                        gesture->type = TOUCH_EVENT_MOVE;
                        gesture->x = ev.value;
                        gesture->y = state->points[i].y;
                        break;
                    }
                }
                break;
            }

            case ABS_MT_POSITION_Y: {
                for (int i = MAX_TOUCH_POINTS - 1; i >= 0; i--) {
                    if (state->points[i].active) {
                        state->points[i].y = ev.value;
                        state->points[i].last_time_ms = get_time_ms();
                        gesture->type = TOUCH_EVENT_MOVE;
                        gesture->x = state->points[i].x;
                        gesture->y = ev.value;
                        break;
                    }
                }
                break;
            }
            }
            break;

        case EV_SYN:
            /* SYN_REPORT signals end of a complete touch frame */
            if (ev.code == SYN_REPORT) {
                /* Frame complete — could do per-frame processing here */
            }
            break;
        }
    }

    return events_processed;
}

/* ================================================================
 *  Standalone Demo: Monitor touch events and print gestures
 * ================================================================ */

#ifdef TOUCH_INPUT_STANDALONE

static const char *event_name(touch_event_type_t type)
{
    switch (type) {
    case TOUCH_EVENT_DOWN:        return "DOWN";
    case TOUCH_EVENT_UP:          return "UP";
    case TOUCH_EVENT_MOVE:        return "MOVE";
    case TOUCH_EVENT_TAP:         return "TAP";
    case TOUCH_EVENT_DOUBLE_TAP:  return "DOUBLE_TAP";
    case TOUCH_EVENT_LONG_PRESS:  return "LONG_PRESS";
    case TOUCH_EVENT_SWIPE_LEFT:  return "SWIPE_LEFT";
    case TOUCH_EVENT_SWIPE_RIGHT: return "SWIPE_RIGHT";
    case TOUCH_EVENT_SWIPE_UP:    return "SWIPE_UP";
    case TOUCH_EVENT_SWIPE_DOWN:  return "SWIPE_DOWN";
    default:                      return "NONE";
    }
}

int main(int argc, char *argv[])
{
    touch_state_t ts;
    const char *dev = (argc > 1) ? argv[1] : NULL;

    printf("=== RK3568 Touch Input Demo ===\n");
    printf("Tap the screen to test. Ctrl+C to exit.\n");

    if (touch_init(&ts, dev) < 0) {
        return 1;
    }

    /* Switch to blocking mode for the demo */
    int flags = fcntl(ts.fd, F_GETFL, 0);
    fcntl(ts.fd, F_SETFL, flags & ~O_NONBLOCK);

    printf("[demo] Listening for touch events...\n");

    while (1) {
        touch_gesture_t gesture;
        int n = touch_poll(&ts, &gesture);

        if (n < 0) {
            fprintf(stderr, "[demo] Poll error, exiting.\n");
            break;
        }

        if (gesture.type != TOUCH_EVENT_NONE) {
            printf("[demo] Event: %s at (%d, %d)",
                   event_name(gesture.type), gesture.x, gesture.y);
            if (gesture.dx != 0 || gesture.dy != 0) {
                printf(" delta=(%d,%d)", gesture.dx, gesture.dy);
            }
            printf("\n");
        }
    }

    touch_release(&ts);
    return 0;
}

#endif /* TOUCH_INPUT_STANDALONE */
