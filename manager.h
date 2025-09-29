#ifndef MANAGER_H
#define MANAGER_H

#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

/*
    single threaded manager process to support management 
    of the processes implementing k >= 1 DSS
*/

#define COMMAND_SIZE 1024
#define MAX_PEERS 100
#define MAX_DSS 10
#define NAME_LEN 16

typedef struct {
    char name[NAME_LEN];
    char ipv4_addr[16];
    uint16_t mport;
    uint16_t cport;
} User;

typedef enum {
    FREE,
    IN_DSS
} DiskState;

typedef struct {
    char name[NAME_LEN];
    char ipv4_addr[16];
    uint16_t mport;
    uint16_t cport;
    DiskState state;
    char dss_name[NAME_LEN];
} Disk;

typedef struct {
    char name[NAME_LEN];
    int n;
    uint32_t striping_unit;
    int disk_indices[MAX_PEERS];
} DSS;

// command handler that will handle the incoming commands
// return 0 is success else -1
int command_handler(char *, struct sockaddr_in*, socklen_t);

// registers user inside manager's state
// return 0 is success else -1
int register_user(char*, char*, uint16_t, uint16_t);

// registers disk inside manager's state
// return 0 is success else -1
int register_disk(char*, char*, uint16_t, uint16_t);

int configure_dss(char*, int n, uint32_t stripping_unit);

#endif // MANAGER_H
