#include "user.h"
#include "logging.h"
#include "manager.h"
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static char USERNAME[USERNAME_LEN];
static struct sockaddr_in MANAGER_ADDR;
static uint16_t MPORT;
static uint16_t CPORT;

static int MANAGER_SOCK = -1;
static int COPY_SOCK = -1;

static char LAST_COPY_FILE[NAME_LEN];
static unsigned int LAST_COPY_SIZE;
static char LAST_COPY_OWNER[NAME_LEN];

int get_local_ip(char *buffer, size_t buflen) {
    struct ifaddrs *ifaddr, *ifa;
    int found = 0;

    if (getifaddrs(&ifaddr) == -1) {
        strncpy(buffer, "127.0.0.1", buflen - 1);
        return -1;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;

        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
            char *ip = inet_ntoa(addr->sin_addr);
            
            if (strncmp(ip, "127.", 4) != 0) {
                strncpy(buffer, ip, buflen - 1);
                buffer[buflen - 1] = '\0';
                found = 1;
                break;
            }
        }
    }

    freeifaddrs(ifaddr);
    
    if (!found) {
        strncpy(buffer, "127.0.0.1", buflen - 1);
    }
    
    return found ? 0 : -1;
}

typedef struct {
    struct sockaddr_in dest;
    unsigned char *packet;
    size_t packet_len;
} BlockSendTask;

static void *send_block_thread(void *arg)
{
    BlockSendTask *task = (BlockSendTask *)arg;
    if (!task) {
        return NULL;
    }

    ssize_t wr = sendto(COPY_SOCK, task->packet, task->packet_len, 0,
                        (struct sockaddr *)&task->dest, sizeof(task->dest));
    if (wr < 0) {
        LOG_WARN("sendto failed while writing stripe block");
    } else if ((size_t)wr != task->packet_len) {
        LOG_WARN("Partial send while writing stripe block");
    }

    free(task->packet);
    free(task);
    return NULL;
}

static int copy_params(char *resp)
{
    char dss[NAME_LEN];
    int n;
    unsigned b;
    if (sscanf(resp, "COPY-PARAMS %15s %d %u", dss, &n, &b) != 3) {
        LOG_WARN("FAILED: COPY-PARAMS arguments not correct. response: %s", resp);
        return -1;
    }

    if (n <= 0) {
        LOG_WARN("Invalid disk count received in COPY-PARAMS");
        return -1;
    }

    if (COPY_SOCK < 0) {
        LOG_WARN("Copy socket not initialized");
        return -1;
    }

    typedef struct {
        char name[NAME_LEN];
        char ip[16];
        uint16_t cport;
        struct sockaddr_in addr;
    } Peer;

    Peer peers[16];
    if (n > (int)(sizeof(peers) / sizeof(peers[0]))) {
        LOG_WARN("n is too big");
        return -1;
    }

    char *cursor = resp + 12; // skip COPY-PARAMS
    for (int skip = 0; skip < 3 && *cursor; ) {
        while (*cursor && *cursor != ' ') cursor++;
        while (*cursor == ' ') cursor++;
        skip++;
    }

    for (int i = 0; i < n; i++) {
        if (sscanf(cursor, "%15s %15s %hu",
                   peers[i].name, peers[i].ip, &peers[i].cport) != 3) {
            LOG_WARN("peer info incorrect format @ %d", i);
            return -1;
        }

        for (int skip = 0; skip < 3 && *cursor; ) {
            while (*cursor && *cursor != ' ') cursor++;
            while (*cursor == ' ') cursor++;
            skip++;
        }

        memset(&peers[i].addr, 0, sizeof(peers[i].addr));
        peers[i].addr.sin_family = AF_INET;
        if (inet_pton(AF_INET, peers[i].ip, &peers[i].addr.sin_addr) != 1) {
            LOG_WARN("Invalid peer IP %s", peers[i].ip);
            return -1;
        }
        peers[i].addr.sin_port = htons(peers[i].cport);
    }

    FILE *fp = fopen(LAST_COPY_FILE, "rb");
    if (!fp) {
        LOG_WARN("fopen %s failed", LAST_COPY_FILE);
        return -1;
    }

    unsigned int file_size = LAST_COPY_SIZE;
    unsigned int blocks_per_stripe = (unsigned int)n - 1;
    if (blocks_per_stripe == 0) {
        LOG_WARN("Invalid stripe configuration: no data blocks available");
        fclose(fp);
        return -1;
    }
    unsigned int stripe_bytes = blocks_per_stripe * b;
    unsigned int stripes = (file_size + stripe_bytes - 1) / stripe_bytes;

    if (stripes == 0) {
        stripes = 1;
    }

    unsigned char *stripe = (unsigned char *)malloc(stripe_bytes);
    unsigned char *parity = (unsigned char *)malloc(b);
    if (!stripe || !parity) {
        LOG_WARN("Failed to allocate stripe buffers");
        free(stripe);
        free(parity);
        fclose(fp);
        return -1;
    }

    for (unsigned int s = 0; s < stripes; s++) {
        size_t need = stripe_bytes;
        size_t got = fread(stripe, 1, stripe_bytes, fp);
        if (got < need) {
            memset(stripe + got, 0, need - got);
        }

        memset(parity, 0, b);
        for (unsigned int j = 0; j < blocks_per_stripe; j++) {
            unsigned char *blk = stripe + j * b;
            for (unsigned k = 0; k < b; k++) {
                parity[k] ^= blk[k];
            }
        }

        unsigned int parity_disk = (unsigned int)n - ((s % (unsigned int)n) + 1);
        if (parity_disk >= (unsigned int)n) {
            parity_disk = 0;
        }

        pthread_t thread_ids[16];
        unsigned int thread_count = 0;
        unsigned int data_block_index = 0;

        for (unsigned int d = 0; d < (unsigned int)n; d++) {
            const unsigned char *src_block;
            if (d == parity_disk) {
                src_block = parity;
            } else {
                src_block = stripe + (data_block_index++) * b;
            }

            char header[256];
            int hdrlen = snprintf(header, sizeof(header),
                                  "WRITE %s %s %u %u %s %u\n",
                                  dss, LAST_COPY_FILE, s, d,
                                  (d == parity_disk) ? "PARITY" : "DATA",
                                  b);
            if (hdrlen < 0 || (size_t)hdrlen >= sizeof(header)) {
                LOG_WARN("Failed to construct WRITE header");
                fclose(fp);
                free(parity);
                free(stripe);
                return -1;
            }

            BlockSendTask *task = (BlockSendTask *)malloc(sizeof(BlockSendTask));
            if (!task) {
                LOG_WARN("Failed to allocate block send task");
                fclose(fp);
                free(parity);
                free(stripe);
                return -1;
            }

            size_t packet_len = (size_t)hdrlen + b;
            unsigned char *packet = (unsigned char *)malloc(packet_len);
            if (!packet) {
                LOG_WARN("Failed to allocate packet buffer");
                free(task);
                fclose(fp);
                free(parity);
                free(stripe);
                return -1;
            }

            memcpy(packet, header, (size_t)hdrlen);
            memcpy(packet + hdrlen, src_block, b);

            task->dest = peers[d].addr;
            task->packet = packet;
            task->packet_len = packet_len;
            if (pthread_create(&thread_ids[thread_count], NULL, send_block_thread, task) != 0) {
                LOG_WARN("Failed to spawn writer thread");
                free(packet);
                free(task);
                fclose(fp);
                free(parity);
                free(stripe);
                return -1;
            }
            thread_count++;
        }

        for (unsigned int t = 0; t < thread_count; t++) {
            pthread_join(thread_ids[t], NULL);
        }
    }

    free(parity);
    free(stripe);
    fclose(fp);

    const char *cc = "copy-complete";
    if (sendto(MANAGER_SOCK, cc, strlen(cc), 0,
               (struct sockaddr *)&MANAGER_ADDR, sizeof(MANAGER_ADDR)) < 0) {
        LOG_WARN("Failed to send copy-complete to manager");
        return -1;
    }

    return 0;
}

int receive_cmd(char *cmd_buffer, char *local_ip)
{
    printf("\nAvailable commands:\n");
    printf("  register-user\n");
    printf("  deregister-user\n");
    printf("  configure-dss <dss-name> <n> <striping-unit>\n");
    printf("  copy <file-name> <file-size> <owner>\n");
    printf("  ls\n");
    printf("  exit\n\n");
    printf("user-%s> ", USERNAME);

    size_t cmd_length = COMMAND_SIZE;
    char *line = NULL;
    
    if (getline(&line, &cmd_length, stdin) == -1) {
        free(line);
        LOG_ERROR("error while reading command");
    }
    
    line[strcspn(line, "\n")] = '\0';
    LOG_INFO("received \"%s\" from user", line);
    
    if (strcmp(line, "exit") == 0) {
        free(line);
        return 1;
    }
    
    if (strcmp(line, "register-user") == 0) {
        snprintf(cmd_buffer, COMMAND_SIZE, "register-user %s %s %hu %hu",
                 USERNAME, local_ip, MPORT, CPORT);
    } else if (strcmp(line, "deregister-user") == 0) {
        snprintf(cmd_buffer, COMMAND_SIZE, "deregister-user %s", USERNAME);
    } else if (strncmp(line, "configure-dss", 13) == 0) {
	    strcpy(cmd_buffer, line);
    } else if (strncmp(line, "copy", 4) == 0) {
        char file_name[NAME_LEN];
        unsigned int file_size = 0;
        char owner[NAME_LEN];
        if (sscanf(line, "copy %15s %u %15s", file_name, &file_size, owner) == 3) {
            snprintf(cmd_buffer, COMMAND_SIZE, "copy %s %u %s", file_name, file_size, owner);
            strncpy(LAST_COPY_FILE, file_name, NAME_LEN - 1);
            LAST_COPY_FILE[NAME_LEN - 1] = '\0';
            LAST_COPY_SIZE = file_size;
            strncpy(LAST_COPY_OWNER, owner, NAME_LEN - 1);
            LAST_COPY_OWNER[NAME_LEN - 1] = '\0';
        } else {
            LOG_WARN("Unknown command format for copy");
            free(line);
            return -1;
        }
    } else if (strncmp(line, "ls", 2) == 0) {
        strcpy(cmd_buffer, "ls");
    } else {
        LOG_WARN("Unknown command: %s", line);
        free(line);
        return -1;
    }
    
    free(line);
    return 0;
}

int main(int argc, char* argv[])
{
  if (argc != 6) {
        fprintf(stderr, "Usage: %s <USER NAME> <MANAGER IPv4 ADDRESS> " \
                         "<MANAGER PORT> <MANAGEMENT PORT NUMBER> <COMMAND PORT>\n", argv[0]);
        exit(1);
    }


    char* username = argv[1];
    char* manager_ip = argv[2];
    uint16_t manager_port = atoi(argv[3]);
    MPORT = atoi(argv[4]);
    CPORT = atoi(argv[5]);
    strncpy(USERNAME, username, USERNAME_LEN - 1);
    USERNAME[USERNAME_LEN - 1] = '\0';

    // construct manager sockaddr_in structure
    memset(&MANAGER_ADDR, 0, sizeof(MANAGER_ADDR));
    MANAGER_ADDR.sin_family = AF_INET;
    MANAGER_ADDR.sin_addr.s_addr = inet_addr(manager_ip);
    MANAGER_ADDR.sin_port = htons(manager_port);

    char local_ip[16];
    get_local_ip(local_ip, sizeof(local_ip));
    LOG_INFO("[user name]: %s [local IP]: %s", USERNAME, local_ip);

    LOG_INFO("creating manager socket");
    MANAGER_SOCK = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (MANAGER_SOCK < 0) {
        LOG_ERROR("socket creation failed");
    }
    LOG_INFO("manager socket created successfully.");

    struct sockaddr_in local_m = {0};
    local_m.sin_family = AF_INET;
    local_m.sin_addr.s_addr = htonl(INADDR_ANY);
    local_m.sin_port = htons(MPORT);
    if (bind(MANAGER_SOCK, (struct sockaddr*)&local_m, sizeof(local_m)) < 0) {
        LOG_ERROR("bind() with MPORT failed");
    }

    LOG_INFO("creating copy socket");
    COPY_SOCK = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (COPY_SOCK < 0) {
        LOG_ERROR("copy socket creation failed");
    }

    struct sockaddr_in local_c = {0};
    local_c.sin_family = AF_INET;
    local_c.sin_addr.s_addr = htonl(INADDR_ANY);
    local_c.sin_port = htons(CPORT);
    if (bind(COPY_SOCK, (struct sockaddr*)&local_c, sizeof(local_c)) < 0) {
        LOG_ERROR("bind() with CPORT failed");
    }
    
    char cmd_buffer[COMMAND_SIZE];
    while (1) {
        int cmd = receive_cmd(cmd_buffer, local_ip);

        if (cmd == 1) {
            break;
        } else if (cmd == -1) {
            continue;
        }
        LOG_INFO("sending command \"%s\" to manager", cmd_buffer);

        // send command to manager
        if ( sendto(MANAGER_SOCK, cmd_buffer, strlen(cmd_buffer), 0, (struct sockaddr*) &MANAGER_ADDR, sizeof(MANAGER_ADDR)) != (ssize_t) strlen(cmd_buffer) ) {
            LOG_ERROR("send FAILED. Different number of bytes sent.");
        }

        struct sockaddr_in src_addr;
        socklen_t src_len = sizeof(src_addr);
        char response[RESPONSE_SIZE];
        ssize_t resp_len;

        if ( (resp_len = recvfrom(MANAGER_SOCK, response, RESPONSE_SIZE-1, 0, (struct sockaddr*)&src_addr, &src_len)) < 0) {
            LOG_ERROR("failed to receive response");
        }
        response[resp_len] = '\0';

        if (strncmp(response, "COPY-PARAMS ", 12) == 0) {
            if (copy_params(response) == 0) {
                if ((resp_len = recvfrom(MANAGER_SOCK, response, RESPONSE_SIZE - 1, 0,
                                         (struct sockaddr*)&src_addr, &src_len)) < 0) {
                    LOG_ERROR("failed to receive post-copy response");
                } else {
                    response[resp_len] = '\0';
                    if (MANAGER_ADDR.sin_addr.s_addr != src_addr.sin_addr.s_addr) {
                        LOG_ERROR("Received response from unknown source. MITM possible. Halting");
                    } else {
                        printf("[response]: %s [src ip]: %s\n", response, inet_ntoa(src_addr.sin_addr));
                    }
                }
            } else {
                printf("[response]: FAILURE: Copy aborted locally [src ip]: %s\n",
                       inet_ntoa(src_addr.sin_addr));
            }
            continue;
        }

        // stupid check which is technically very easy to circumvent from attacker's POV
        // TODO: make this more robust
        if (MANAGER_ADDR.sin_addr.s_addr != src_addr.sin_addr.s_addr) {
            LOG_ERROR("Received response from unknown source. MITM possible. Halting");
        }

        printf("[response]: %s [src ip]: %s\n", response, inet_ntoa(src_addr.sin_addr));
    }

    LOG_INFO("Closing socket");
    if (MANAGER_SOCK >= 0) {
        close(MANAGER_SOCK);
    }
    if (COPY_SOCK >= 0) {
        close(COPY_SOCK);
    }
    LOG_INFO("Exiting");
    exit(0);
}
