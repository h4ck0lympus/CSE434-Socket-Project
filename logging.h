#ifndef LOGGING_H
#define LOGGING_H

#define LOG_INFO(msg, ...) \
    fprintf(stderr, "[INFO] %s:%d: " msg "\n", __FILE__, __LINE__, ##__VA_ARGS__)

#define LOG_DEBUG(msg, ...) \
    fprintf(stderr, "[DEBUG] %s:%d:%s(): " msg "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__)

#define LOG_ERROR(msg, ...) \
    do { \
        fprintf(stderr, "[ERROR] %s:%d: " msg "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        exit(EXIT_FAILURE); \
    } while(0)

#endif
