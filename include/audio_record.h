/**
 * audio_record.h - Audio Capture Module for RK3568
 *
 * Wraps Linux ALSA arecord for microphone audio capture.
 * Supports PCM WAV format, configurable sample rate and channels.
 * Provides both direct command-line recording and pipe-based streaming.
 */
#ifndef AUDIO_RECORD_H
#define AUDIO_RECORD_H

#include <stdint.h>

/* Audio format defaults */
#define AUDIO_DEFAULT_RATE      16000   /* 16kHz, standard for ASR */
#define AUDIO_DEFAULT_CHANNELS  1       /* mono */
#define AUDIO_DEFAULT_FORMAT    "S16_LE" /* 16-bit signed little-endian */
#define AUDIO_DEFAULT_DURATION  10      /* seconds */

/* Buffer size for audio streaming */
#define AUDIO_BUFFER_SIZE       (16000 * 2)  /* 1 second of 16kHz 16-bit mono */

/* Maximum WAV file size (10 minutes of 16kHz 16-bit mono) */
#define AUDIO_MAX_FILE_SIZE     (16000 * 2 * 60 * 10)

/* WAV file header (44 bytes) */
#pragma pack(push, 1)
typedef struct {
    /* RIFF header */
    char     riff_id[4];        /* "RIFF" */
    uint32_t riff_size;         /* file size - 8 */
    char     wave_id[4];        /* "WAVE" */
    /* fmt chunk */
    char     fmt_id[4];         /* "fmt " */
    uint32_t fmt_size;          /* 16 for PCM */
    uint16_t audio_format;      /* 1 = PCM */
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;         /* sample_rate * num_channels * bits_per_sample/8 */
    uint16_t block_align;       /* num_channels * bits_per_sample/8 */
    uint16_t bits_per_sample;
    /* data chunk */
    char     data_id[4];        /* "data" */
    uint32_t data_size;
} wav_header_t;
#pragma pack(pop)

/* Audio record state */
typedef struct {
    int    sample_rate;
    int    channels;
    int    bits_per_sample;
    int    duration_sec;
    char   output_file[256];
    char   device[64];          /* ALSA device, e.g. "hw:0,0" or "default" */
    /* streaming state */
    int    is_recording;
    int    pipe_fd;             /* pipe read fd for streaming mode */
    pid_t  arecord_pid;         /* PID of arecord child process */
} audio_state_t;

/* ----- API functions ----- */

/**
 * Initialize the audio state with default settings.
 * Sets sample_rate=16000, channels=1, format=S16_LE, duration=10s.
 */
void audio_init(audio_state_t *state);

/**
 * Set the audio capture parameters.
 */
void audio_config(audio_state_t *state, int rate, int channels, int bits, int duration);

/**
 * Record audio to a WAV file using arecord (blocking).
 * The file is written with a proper WAV header.
 * Returns 0 on success, -1 on error.
 */
int audio_record_wav(audio_state_t *state, const char *output_path);

/**
 * Start recording in streaming mode (non-blocking).
 * Launches arecord as a child process, writes to pipe.
 * Use audio_read_stream() to read captured data.
 * Returns 0 on success, -1 on error.
 */
int audio_record_start(audio_state_t *state);

/**
 * Read a chunk of audio data from the recording stream.
 * Returns bytes read, 0 if no data, -1 on error/EOF.
 */
int audio_read_stream(audio_state_t *state, uint8_t *buffer, int max_len);

/**
 * Stop the streaming recording and clean up.
 */
void audio_record_stop(audio_state_t *state);

/**
 * Write a properly formatted WAV header to an open file descriptor.
 * Call this before writing raw PCM data to create a valid WAV file.
 * Returns 0 on success, -1 on error.
 */
int audio_write_wav_header(int fd, int sample_rate, int channels, int bits_per_sample, uint32_t data_size);

/**
 * Update the data size field in an existing WAV file.
 * Call this after all PCM data has been written.
 */
int audio_finalize_wav(const char *file_path);

/**
 * Build the arecord command-line arguments string.
 * Useful for manual invocation or debugging.
 * Buffer must be at least 512 bytes.
 */
void audio_build_arecord_cmd(audio_state_t *state, char *cmd_buf, int cmd_buf_size);

#endif /* AUDIO_RECORD_H */
