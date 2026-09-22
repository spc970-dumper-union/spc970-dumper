#include "logger.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sio.h>

// A full ROM dump with per-chunk logging can produce well over 64KB of log
// text; the old 64KB cap silently dropped everything past it, including
// restore-phase errors at the end of the run (see spc970-dumper audit L-4).
#define LOG_BUFFER_SIZE (256 * 1024)

static char s_log_buffer[LOG_BUFFER_SIZE];
static int s_log_len = 0;
static int s_log_truncated = 0;

void log_init(void) {
    memset(s_log_buffer, 0, sizeof(s_log_buffer));
    s_log_len = 0;
    s_log_truncated = 0;
    sio_init(115200, 0, 0, 0, 0);
    log_printf("\n=====================================================\n");
    log_printf("=== PS2 MechaCon Tool Debug Console Log Active    ===\n");
    log_printf("=====================================================\n");
}

void log_printf(const char *fmt, ...) {
    char msg[1024];
    va_list args;

    va_start(args, fmt);
    int written = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    if (written <= 0) return;

    // 1. Output directly to EE stdout / SIO / host terminal / emulator console
    sio_puts(msg);
    printf("%s", msg);
    fflush(stdout);

    // 2. Append to internal buffer for USB DEBUG_LOG.TXT
    int remaining = LOG_BUFFER_SIZE - s_log_len - 1;
    if (remaining > 0) {
        if (written > remaining) written = remaining;
        memcpy(&s_log_buffer[s_log_len], msg, written);
        s_log_len += written;
        s_log_buffer[s_log_len] = '\0';
    } else if (!s_log_truncated) {
        // Buffer is full: leave a visible marker instead of silently dropping
        // everything from here on (e.g. NVRAM restore results at dump end).
        s_log_truncated = 1;
        static const char marker[] = "\n[LOG_TRUNCATED] Buffer full - later entries were not saved to DEBUG_LOG.TXT\n";
        sio_puts(marker);
        printf("%s", marker);
    }
}

int log_save_to_file(const char *filepath) {
    if (!filepath || s_log_len == 0) return -1;

    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return -1;

    int written = write(fd, s_log_buffer, s_log_len);
    close(fd);

    return (written == s_log_len) ? 0 : -1;
}

const char *log_get_buffer(void) {
    return s_log_buffer;
}
