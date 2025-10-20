#ifndef MANAGER_H
#define MANAGER_H

#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

/*
    single threaded manager process to support management 
    of the processes implementing k >= 1 DSS
*/

#define COMMAND_SIZE 1024
#define MAX_PEERS 100
#define MAX_DSS 10
#define NAME_LEN 16
#define MAX_FILES 1000

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
    uint64_t size;
    char owner[NAME_LEN];
} File;

typedef struct {
    char name[NAME_LEN];
    int n; // number of disks in disk array
    uint32_t striping_unit;
    int disk_indices[MAX_PEERS];
    File files[MAX_FILES];
    int file_count;
} DSS;

typedef struct {
    char file_name[NAME_LEN];
    int file_size;
    char owner[NAME_LEN];
    int dss_idx;
    int is_pending;
} PendingCopy;

typedef struct {
    int active;
    int dss_idx;
    char file[NAME_LEN];
    char user[NAME_LEN];
    size_t size;
} PendingRead;

typedef struct {
    char dss_name[NAME_LEN];
    int dss_idx;
    int is_pending;
} PendingDiskFailure;

typedef struct {
    char dss_name[NAME_LEN];
    int dss_idx;
    int is_pending;
} PendingDecommission;

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

int ls(char* response_buffer, size_t buffer_size);

int copy(char* file_name, int file_size, char* owner);

int read_file(char* file_name, char* user_name);

int disk_failure(char* dss_name);

int decommission_dss(char* dss_name);

#endif // MANAGER_H
