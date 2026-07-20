/**
 * audio_record.c - Audio Capture Module for RK3568
 *
 * Wraps ALSA arecord for microphone audio capture.
 * Supports WAV file recording (blocking) and pipe-based streaming (non-blocking).
 * Output is 16kHz 16-bit mono PCM — standard format for speech recognition engines.
 *
 * Compile: aarch64-linux-gcc -o audio_record audio_record.c -static
 * Usage:   ./audio_record [output.wav] [duration_sec]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#include "audio_record.h"

/* ================================================================
 *  Initialization
 * ================================================================ */

void audio_init(audio_state_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(audio_state_t));
    state->sample_rate = AUDIO_DEFAULT_RATE;
    state->channels = AUDIO_DEFAULT_CHANNELS;
    state->bits_per_sample = 16;
    state->duration_sec = AUDIO_DEFAULT_DURATION;
    strncpy(state->device, "default", sizeof(state->device) - 1);
    state->is_recording = 0;
    state->pipe_fd = -1;
    state->arecord_pid = -1;
}

void audio_config(audio_state_t *state, int rate, int channels, int bits, int duration)
{
    if (!state) return;
    if (rate > 0) state->sample_rate = rate;
    if (channels > 0) state->channels = channels;
    if (bits > 0) state->bits_per_sample = bits;
    if (duration > 0) state->duration_sec = duration;
}

/* ================================================================
 *  WAV Header Utilities
 * ================================================================ */

int audio_write_wav_header(int fd, int sample_rate, int channels,
                            int bits_per_sample, uint32_t data_size)
{
    wav_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));

    int bytes_per_sample = bits_per_sample / 8;

    /* RIFF header */
    memcpy(hdr.riff_id, "RIFF", 4);
    hdr.riff_size = 36 + data_size;
    memcpy(hdr.wave_id, "WAVE", 4);

    /* fmt chunk */
    memcpy(hdr.fmt_id, "fmt ", 4);
    hdr.fmt_size = 16;
    hdr.audio_format = 1;  /* PCM */
    hdr.num_channels = channels;
    hdr.sample_rate = sample_rate;
    hdr.byte_rate = sample_rate * channels * bytes_per_sample;
    hdr.block_align = channels * bytes_per_sample;
    hdr.bits_per_sample = bits_per_sample;

    /* data chunk */
    memcpy(hdr.data_id, "data", 4);
    hdr.data_size = data_size;

    ssize_t written = write(fd, &hdr, sizeof(hdr));
    if (written != sizeof(hdr)) {
        fprintf(stderr, "[audio] ERROR: Failed to write WAV header (%zd of %zu bytes)\n",
                written, sizeof(hdr));
        return -1;
    }

    return 0;
}

int audio_finalize_wav(const char *file_path)
{
    if (!file_path) return -1;

    /* Open the file for reading and writing */
    int fd = open(file_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[audio] ERROR: Cannot open %s for finalization: %s\n",
                file_path, strerror(errno));
        return -1;
    }

    /* Determine total file size */
    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size < 0) {
        fprintf(stderr, "[audio] ERROR: lseek failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* Go back to beginning */
    lseek(fd, 0, SEEK_SET);

    /* Read existing header */
    wav_header_t hdr;
    if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        fprintf(stderr, "[audio] ERROR: Failed to read WAV header.\n");
        close(fd);
        return -1;
    }

    /* Update RIFF and data sizes */
    uint32_t data_size = (uint32_t)(file_size - sizeof(hdr));
    hdr.riff_size = 36 + data_size;
    hdr.data_size = data_size;

    /* Write corrected header */
    lseek(fd, 0, SEEK_SET);
    if (write(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        fprintf(stderr, "[audio] ERROR: Failed to write corrected WAV header.\n");
        close(fd);
        return -1;
    }

    close(fd);
    printf("[audio] WAV file finalized: %s (data size: %u bytes)\n",
           file_path, data_size);
    return 0;
}

/* ================================================================
 *  Command Line Builder
 * ================================================================ */

void audio_build_arecord_cmd(audio_state_t *state, char *cmd_buf, int cmd_buf_size)
{
    if (!state || !cmd_buf) return;

    const char *format_str = "S16_LE";
    switch (state->bits_per_sample) {
    case 8:  format_str = "U8"; break;
    case 16: format_str = "S16_LE"; break;
    case 24: format_str = "S24_3LE"; break;
    case 32: format_str = "S32_LE"; break;
    }

    snprintf(cmd_buf, cmd_buf_size,
             "arecord -D %s -f %s -r %d -c %d -d %d -t wav",
             state->device, format_str, state->sample_rate,
             state->channels, state->duration_sec);

    /* Append output file if specified */
    if (state->output_file[0] != '\0') {
        int remaining = cmd_buf_size - strlen(cmd_buf) - 1;
        if (remaining > 2) {
            strncat(cmd_buf, " ", remaining);
            strncat(cmd_buf, state->output_file, remaining - 1);
        }
    }
}

/* ================================================================
 *  Blocking WAV Recording
 * ================================================================ */

int audio_record_wav(audio_state_t *state, const char *output_path)
{
    if (!state || !output_path) return -1;

    printf("[audio] Recording %d seconds of audio to %s...\n",
           state->duration_sec, output_path);
    printf("[audio] Format: %d Hz, %d channel(s), %d-bit\n",
           state->sample_rate, state->channels, state->bits_per_sample);

    /* Build arecord command */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "arecord -D %s -f S16_LE -r %d -c %d -d %d -t wav %s 2>&1",
             state->device, state->sample_rate, state->channels,
             state->duration_sec, output_path);

    /* Execute arecord */
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "[audio] ERROR: popen failed: %s\n", strerror(errno));
        fprintf(stderr, "[audio] HINT: Make sure 'arecord' is installed "
                "(apt install alsa-utils).\n");
        return -1;
    }

    /* Read and display any error output from arecord */
    char buf[256];
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        printf("[audio:arecord] %s", buf);
    }

    int status = pclose(fp);
    if (status != 0) {
        fprintf(stderr, "[audio] ERROR: arecord exited with status %d\n", status);
        fprintf(stderr, "[audio] HINT: Check microphone connection and ALSA mixer "
                "settings ('alsamixer').\n");
        return -1;
    }

    printf("[audio] Recording complete: %s\n", output_path);
    return 0;
}

/* ================================================================
 *  Streaming (Non-blocking) Recording
 * ================================================================ */

int audio_record_start(audio_state_t *state)
{
    if (!state || state->is_recording) return -1;

    /* Create a pipe for communication */
    int pipefds[2];
    if (pipe(pipefds) < 0) {
        fprintf(stderr, "[audio] ERROR: pipe() failed: %s\n", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[audio] ERROR: fork() failed: %s\n", strerror(errno));
        close(pipefds[0]);
        close(pipefds[1]);
        return -1;
    }

    if (pid == 0) {
        /* ---- Child process: run arecord ---- */

        /* Close read end of pipe */
        close(pipefds[0]);

        /* Redirect stdout to the pipe */
        dup2(pipefds[1], STDOUT_FILENO);
        close(pipefds[1]);

        /* Build and execute arecord, outputting raw PCM to stdout */
        execlp("arecord", "arecord",
               "-D", state->device,
               "-f", "S16_LE",
               "-r", (char[]){0},
               "-c", (char[]){0},
               "-t", "raw",
               (char *)NULL);

        /* This block runs on failure; but we can't pass ints via execlp for -r/-c
         * cleanly, so build command with snprintf and use system() instead */
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "arecord -D %s -f S16_LE -r %d -c %d -t raw 2>/dev/null",
                 state->device, state->sample_rate, state->channels);

        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);

        /* If we get here, execl failed */
        fprintf(stderr, "[audio] Child: execl failed: %s\n", strerror(errno));
        _exit(1);
    }

    /* ---- Parent process ---- */

    /* Close write end of pipe */
    close(pipefds[1]);

    state->pipe_fd = pipefds[0];
    state->arecord_pid = pid;
    state->is_recording = 1;

    /* Set pipe to non-blocking */
    int flags = fcntl(state->pipe_fd, F_GETFL, 0);
    fcntl(state->pipe_fd, F_SETFL, flags | O_NONBLOCK);

    printf("[audio] Streaming recording started (pid=%d)\n", pid);
    return 0;
}

int audio_read_stream(audio_state_t *state, uint8_t *buffer, int max_len)
{
    if (!state || !state->is_recording || state->pipe_fd < 0) return -1;

    ssize_t n = read(state->pipe_fd, buffer, max_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        fprintf(stderr, "[audio] ERROR: read stream: %s\n", strerror(errno));
        return -1;
    }
    if (n == 0) {
        /* EOF — recording process ended */
        state->is_recording = 0;
        return -1;
    }
    return (int)n;
}

void audio_record_stop(audio_state_t *state)
{
    if (!state || !state->is_recording) return;

    /* Send SIGTERM to arecord process */
    if (state->arecord_pid > 0) {
        kill(state->arecord_pid, SIGTERM);

        /* Wait for the child to exit (non-blocking poll then wait) */
        int status;
        int waited = 0;
        for (int i = 0; i < 30; i++) {  /* 3 second max wait */
            pid_t ret = waitpid(state->arecord_pid, &status, WNOHANG);
            if (ret > 0) { waited = 1; break; }
            if (ret < 0) break;
            usleep(100000);  /* 100ms */
        }
        if (!waited) {
            kill(state->arecord_pid, SIGKILL);
            waitpid(state->arecord_pid, &status, 0);
        }
    }

    /* Close the pipe */
    if (state->pipe_fd >= 0) {
        close(state->pipe_fd);
        state->pipe_fd = -1;
    }

    state->is_recording = 0;
    state->arecord_pid = -1;
    printf("[audio] Recording stopped.\n");
}

/* ================================================================
 *  Standalone Demo: Record audio and save as WAV
 * ================================================================ */

#ifdef AUDIO_RECORD_STANDALONE

int main(int argc, char *argv[])
{
    audio_state_t audio;

    const char *output = (argc > 1) ? argv[1] : "test_recording.wav";
    int duration = (argc > 2) ? atoi(argv[2]) : 5;

    printf("=== RK3568 Audio Record Demo ===\n");

    audio_init(&audio);
    audio.duration_sec = duration;

    printf("[demo] Recording %d seconds to %s...\n", duration, output);
    printf("[demo] Speak into the microphone now.\n");

    if (audio_record_wav(&audio, output) < 0) {
        fprintf(stderr, "[demo] Recording failed.\n");
        return 1;
    }

    printf("[demo] Done! File: %s\n", output);
    printf("[demo] Play back with: aplay %s\n", output);
    return 0;
}

#endif /* AUDIO_RECORD_STANDALONE */
