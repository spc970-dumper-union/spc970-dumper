#ifndef LOGGER_H
#define LOGGER_H

void log_init(void);
void log_printf(const char *fmt, ...);
int log_save_to_file(const char *filepath);
const char *log_get_buffer(void);

#endif // LOGGER_H
