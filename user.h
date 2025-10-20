#ifndef USER_H
#define USER_H
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <pthread.h>
#include <stdio.h>

#include "logging.h"

#define COMMAND_SIZE 1024
#define RESPONSE_SIZE 1024

#define USERNAME_LEN 16
#define MAX_DISKS 100
#define NAME_LEN 16

typedef struct {
    char name[NAME_LEN];
    char ip[16];
    uint16_t cport;
} DiskInfo;

typedef struct {
    char name[NAME_LEN];
    int n;
    uint32_t striping_unit;
    DiskInfo disks[MAX_DISKS];
} DSSInfo;

typedef struct {
    struct sockaddr_in dest;
    char *request;
    size_t request_len;
    unsigned char **block_data;
    size_t *block_size;
    int stripe_num;
    int disk_idx;
    int success;
} BlockReadTask;

typedef struct {
    struct sockaddr_in dest;
    unsigned char *packet;
    size_t packet_len;
} BlockSendTask;

typedef struct {
    char name[NAME_LEN];
    char ip[16];
    uint16_t cport;
    struct sockaddr_in addr;
} Peer;

typedef struct {
    int disk_idx;
    char *block_data;
    size_t block_size;
    char disk_ip[16];
    uint16_t disk_port;
    char file_name[NAME_LEN];
    int stripe_num;
    int block_type; // 0=data, 1=parity
} ThreadArgs;

int perform_copy(char *file_name, int file_size, char *owner, int sock);

#endif
