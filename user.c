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

static void copy_params(char* resp, int sock)
{
    char dss[NAME_LEN]; 
    int n;
    unsigned b;
    if (sscanf(resp, "COPY-PARAMS %s %d %u", dss, &n, &b) != 3) {
        LOG_INFO("FAILED: COPY-PARAMS arguments not correct. response: %s", resp);
        return;
    }
    
    resp += 12; // skip COPY-PARAMS

    // skip "%s %d %u"
    for (int t=0; t<3; ) {
        while (*resp && *resp != ' ') resp++;
        while (*resp == ' ') resp++;
        t++;
    }

    typedef struct {
        char name[NAME_LEN];
        char ip[16];
        uint16_t cport;
        struct sockaddr_in addr;
    } Peer;

    Peer peers[16];
    if (n > 16) {
        LOG_INFO("n is too big");
        return;
    }

    for (int i=0; i < n; i++) {
        if (sscanf(resp, "%s %15s %hu", 
                   peers[i].name, peers[i].ip, &peers[i].cport) != 3) {
            LOG_INFO("peer info incorrect format @ %d", i);
            return;
        }

        // skip "%s %d %u"
        for (int t=0; t<3; ) {
            while (*resp && *resp != ' ') resp++;
            while (*resp == ' ') resp++;
            t++;
        }

        memset(&peers[i].addr, 0, sizeof(peers[i].addr));
        peers[i].addr.sin_family = AF_INET;
        inet_pton(AF_INET, peers[i].ip, &peers[i].addr.sin_addr);
        peers[i].addr.sin_port = htons(peers[i].cport);
    }

    // open the source file
    FILE* fp = fopen(LAST_COPY_FILE, "rb");
    if (!fp) {
        LOG_ERROR("fopen %s failed", LAST_COPY_FILE);
    }

    unsigned int file_size = LAST_COPY_SIZE;
    unsigned int blocks_per_stripe = n - 1;
    unsigned int stripe_bytes = blocks_per_stripe * b;
    unsigned int stripes = (file_size + stripe_bytes -1 ) / stripe_bytes;

    unsigned char* stripe = malloc(stripe_bytes);
    unsigned char* parity = malloc(b);

    int p2p_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (p2p_sock < 0) {
        LOG_ERROR("Failed to open p2p UDP socket");
    }


    for (unsigned int s=0; s<stripes; s++) {
        size_t need = stripe_bytes;
        size_t got = fread(stripe, 1, stripe_bytes, fp);
        // handle the padding of null bytes
        if (got < need) {
            memset(stripe+got, 0, need-got);
        }

        // parity = XOR of (n-1) blocks, each size b
        memset(parity, 0, b);
        for (unsigned int j=0; j < blocks_per_stripe; j++) {
            unsigned char* blk = stripe + j*b;
            for (unsigned k=0; k < b; k++) {
                parity[k] ^= blk[k];
            }
        }

        // picking parity disk
        unsigned int parity_disk = (unsigned int)n - ((s % (unsigned int)n) + 1);
        if (parity_disk >= (unsigned int)n)
            parity_disk = 0;

        for (unsigned int d=0, dj=0; d < (unsigned int)n; d++) {
            const void* blk_ptr;
            if (d == parity_disk) {
                blk_ptr = parity;
            } else {
                blk_ptr = stripe + (dj++) * b; // mapping next data block
            }

            // WRITE <dss> <file> <stripe> <disk> <type> <b>\n<raw bytes>
            char header[256];
            int hdrlen = snprintf(header, sizeof(header),
                                  "WRITE %s %s %u %u %s %u\n",
                                  dss, LAST_COPY_FILE, s, d,
                                  (d == parity_disk) ? "PARITY": "DATA",
                                  b
            );

            size_t pkt_len = (size_t)hdrlen + b;
            unsigned char* pkt = (unsigned char*)malloc(pkt_len);
            if (!pkt) {
                LOG_ERROR("malloc failed for UDP packet");
            }

            memcpy(pkt, header, hdrlen);
            memcpy(pkt+hdrlen, blk_ptr, b);
            ssize_t wr = sendto(p2p_sock, pkt, pkt_len, 0, (struct sockaddr*)&peers[d].addr, sizeof(peers[d].addr));
            if (wr < 0) {
                LOG_ERROR("sendto failed - copy - raw bytes");
            }
            free(pkt);
        }
    }

    free(parity);
    free(stripe);
    fclose(fp);

    // done with phase-2 of copy. Send copy-complete
    const char *cc = "copy-complete";
    sendto(sock, cc, strlen(cc), 0, (struct sockaddr*)&MANAGER_ADDR, sizeof(MANAGER_ADDR));
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
        int file_size = 0;
        char owner[NAME_LEN];
        if (snprintf(cmd_buffer, COMMAND_SIZE, "copy %s %d %s", file_name, file_size, owner) == 3) {
            strcpy(LAST_COPY_FILE, file_name);
            LAST_COPY_SIZE = file_size;
            strcpy(LAST_COPY_OWNER, owner);
        } else {
            LOG_INFO("Unknown command format for copy");
        }
    } else if (strncmp(line, "ls", 2) == 0) {
        strcpy(cmd_buffer, "ls");
    } else {
        LOG_INFO("Unknown command: %s", line);
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

    LOG_INFO("creating new socket");
    int sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        LOG_ERROR("socket creation failed");
    }
    LOG_INFO("socket created successfully.");

    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(MPORT);
    if (bind(sock, (struct sockaddr*)&local, sizeof(local)) < 0) {
        LOG_ERROR("bind() with MPORT failed");
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
        if ( sendto(sock, cmd_buffer, strlen(cmd_buffer), 0, (struct sockaddr*) &MANAGER_ADDR, sizeof(MANAGER_ADDR)) != (ssize_t) strlen(cmd_buffer) ) {
            LOG_ERROR("send FAILED. Different number of bytes sent.");
        }

        struct sockaddr_in src_addr;
        socklen_t src_len = sizeof(src_addr);
        char response[RESPONSE_SIZE];
        ssize_t resp_len;

        if ( (resp_len = recvfrom(sock, response, RESPONSE_SIZE-1, 0, (struct sockaddr*)&src_addr, &src_len)) < 0) {
            LOG_ERROR("failed to receive response");
        }
        response[resp_len] = '\0';

        if (strncmp(response, "COPY-PARAMS ", 12) == 0) {
            copy_params(response, sock);
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
    close(sock);
    LOG_INFO("Exiting");
    exit(0);
}
