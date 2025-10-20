#ifndef DISK_H
#define DISK_H
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>

#include "logging.h"

#define COMMAND_SIZE 1024
#define RESPONSE_SIZE 1024

#define DISK_NAME_LEN 16
#ifndef NAME_LEN
#define NAME_LEN 16
#endif

#include <pthread.h>

#endif
