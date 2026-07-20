/**
 * touch_input.h - RK3568 Touchscreen Input Driver
 *
 * Reads raw touch events from Linux input subsystem (/dev/input/event*).
 * Parses ABS_MT_POSITION_X/Y for multi-touch coordinates,
 * detects tap, swipe, and long-press gestures.
 */
#ifndef TOUCH_INPUT_H
#define TOUCH_INPUT_H

#include <linux/input.h>
#include <stdint.h>

/* Maximum touch points to track */
#define MAX_TOUCH_POINTS    5

/* Gesture detection thresholds */
#define SWIPE_MIN_DISTANCE  50      /* pixels */
#define TAP_MAX_TIME_MS     200     /* milliseconds */
#define TAP_MAX_DISTANCE    20      /* pixels */
#define LONG_PRESS_TIME_MS  800     /* milliseconds */

/* Touch event types */
typedef enum {
    TOUCH_EVENT_NONE = 0,
    TOUCH_EVENT_DOWN,          /* finger down */
    TOUCH_EVENT_UP,            /* finger up */
    TOUCH_EVENT_MOVE,          /* finger moving */
    TOUCH_EVENT_TAP,           /* quick tap detected */
    TOUCH_EVENT_DOUBLE_TAP,    /* double tap */
    TOUCH_EVENT_LONG_PRESS,    /* long press */
    TOUCH_EVENT_SWIPE_LEFT,
    TOUCH_EVENT_SWIPE_RIGHT,
    TOUCH_EVENT_SWIPE_UP,
    TOUCH_EVENT_SWIPE_DOWN,
} touch_event_type_t;

/* A single touch point */
typedef struct {
    int id;                    /* tracking ID from input subsystem */
    int x;                     /* current X coordinate */
    int y;                     /* current Y coordinate */
    int start_x;               /* X at touch-down */
    int start_y;               /* Y at touch-down */
    int active;                /* 1 = currently pressed */
    uint64_t start_time_ms;    /* touch-down timestamp */
    uint64_t last_time_ms;     /* last event timestamp */
} touch_point_t;

/* Gesture result after processing a batch of raw events */
typedef struct {
    touch_event_type_t type;
    int x;                     /* event X coordinate */
    int y;                     /* event Y coordinate */
    int dx;                    /* swipe delta X */
    int dy;                    /* swipe delta Y */
} touch_gesture_t;

/* Global touch state */
typedef struct {
    int fd;                              /* input device fd */
    char device_path[256];               /* e.g. /dev/input/event1 */
    touch_point_t points[MAX_TOUCH_POINTS];
    int point_count;
    int screen_w;
    int screen_h;
    /* double-tap tracking */
    uint64_t last_tap_time_ms;
    int last_tap_x;
    int last_tap_y;
    int double_tap_pending;
} touch_state_t;

/* ----- API functions ----- */

/**
 * Open and initialize the touch input device.
 * Tries auto-detection of the touch device (searches /dev/input/event*).
 * Set device_path to a specific path or NULL for auto-detect.
 * Returns 0 on success, -1 on failure.
 */
int touch_init(touch_state_t *state, const char *device_path);

/**
 * Close the touch input device.
 */
void touch_release(touch_state_t *state);

/**
 * Read one raw input_event from the device (blocking by default).
 * Returns 0 on success, -1 on error/EOF.
 */
int touch_read_raw(touch_state_t *state, struct input_event *ev);

/**
 * Read and process pending touch events (non-blocking poll).
 * Detects gestures and fills the gesture struct.
 * Returns number of events processed, or -1 on error.
 */
int touch_poll(touch_state_t *state, touch_gesture_t *gesture);

/**
 * Map raw touch coordinates to screen coordinates.
 * Handles any scaling or axis inversion needed.
 */
void touch_map_coords(touch_state_t *state, int raw_x, int raw_y, int *screen_x, int *screen_y);

/**
 * Check if a touch point is within a given rectangular region.
 * Useful for button hit-testing.
 */
int touch_in_rect(int tx, int ty, int rx, int ry, int rw, int rh);

#endif /* TOUCH_INPUT_H */
