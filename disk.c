#include "disk.h"
#include "logging.h"

static char DISK_NAME[DISK_NAME_LEN];
static struct sockaddr_in MANAGER_ADDR;
static uint16_t MPORT;
static uint16_t CPORT;

static int MANAGER_SOCK = -1;
static int COPY_SOCK = -1;
static volatile sig_atomic_t COPY_THREAD_ACTIVE = 1;
static pthread_t COPY_THREAD;
static int COPY_THREAD_STARTED = 0;

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

static int ensure_directory(const char *path)
{
    if (mkdir(path, 0775) == -1) {
        if (errno == EEXIST) {
            return 0;
        }
        return -1;
    }
    return 0;
}

static int persist_block(const char *dss, const char *file_name, unsigned int stripe,
                         unsigned int disk_idx, const char *block_type,
                         const unsigned char *payload, size_t payload_len)
{
    const char *root = "storage";
    char path[512];

    if (ensure_directory(root) < 0) {
        LOG_WARN("Failed to create storage root directory");
        return -1;
    }

    snprintf(path, sizeof(path), "%s/%s", root, DISK_NAME);
    if (ensure_directory(path) < 0) {
        LOG_WARN("Failed to create disk storage directory");
        return -1;
    }

    snprintf(path, sizeof(path), "%s/%s/%s", root, DISK_NAME, dss);
    if (ensure_directory(path) < 0) {
        LOG_WARN("Failed to create DSS storage directory");
        return -1;
    }

    char file_path[512];
    snprintf(file_path, sizeof(file_path), "%s/%s/%s/%s_stripe%u_disk%u_%s.bin",
             root, DISK_NAME, dss, file_name, stripe, disk_idx, block_type);

    FILE *fp = fopen(file_path, "wb");
    if (!fp) {
        LOG_WARN("Failed to open %s for writing", file_path);
        return -1;
    }

    size_t written = fwrite(payload, 1, payload_len, fp);
    fclose(fp);
    if (written != payload_len) {
        LOG_WARN("Short write when persisting block to %s", file_path);
        return -1;
    }

    LOG_INFO("Stored block %s stripe %u disk %u (%zu bytes)",
             file_name, stripe, disk_idx, payload_len);
    return 0;
}

static void *copy_listener(void *arg)
{
    (void)arg;
    const size_t buf_size = 2048 + 2 * 1024 * 1024;
    unsigned char *buffer = (unsigned char *)malloc(buf_size);
    if (!buffer) {
        LOG_WARN("Failed to allocate copy listener buffer");
        return NULL;
    }

    while (COPY_THREAD_ACTIVE) {
        struct sockaddr_in src_addr;
        socklen_t src_len = sizeof(src_addr);
        ssize_t recv_len = recvfrom(COPY_SOCK, buffer, buf_size, 0,
                                    (struct sockaddr *)&src_addr, &src_len);
        if (recv_len < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!COPY_THREAD_ACTIVE) {
                break;
            }
            LOG_WARN("recvfrom on copy socket failed");
            continue;
        }

        if (recv_len == 0) {
            continue;
        }

        unsigned char *newline = memchr(buffer, '\n', (size_t)recv_len);
        if (!newline) {
            LOG_INFO("Malformed WRITE command received");
            continue;
        }

        size_t header_len = (size_t)(newline - buffer);
        size_t payload_len = (size_t)recv_len - header_len - 1;
        buffer[header_len] = '\0';

        char dss[NAME_LEN];
        char file_name[NAME_LEN];
        unsigned int stripe = 0;
        unsigned int disk_idx = 0;
        char block_type[8];
        unsigned int block_size = 0;

        // cHeck if this is a WRITE command
        if (strncmp((char *)buffer, "WRITE ", 6) == 0) {
            char dss[NAME_LEN];
            char file_name[NAME_LEN];
            unsigned int stripe = 0;
            unsigned int disk_idx = 0;
            char block_type[8];
            unsigned int block_size = 0;

            if (sscanf((char *)buffer, "WRITE %15s %15s %u %u %7s %u",
                       dss, file_name, &stripe, &disk_idx, block_type, &block_size) != 6) {
                LOG_INFO("Failed to parse WRITE header: %s", buffer);
                continue;
            }

            if (payload_len != block_size) {
                LOG_INFO("WRITE payload length mismatch: expected %u, got %zu",
                         block_size, payload_len);
                continue;
            }

            const unsigned char *payload = newline + 1;
            persist_block(dss, file_name, stripe, disk_idx, block_type, payload, payload_len);
        }
        // check if this is a READ command
        else if (strncmp((char *)buffer, "READ ", 5) == 0) {
            char dss[NAME_LEN];
            char file_name[NAME_LEN];
            unsigned int stripe = 0;
            unsigned int disk_idx = 0;

            if (sscanf((char *)buffer, "READ %15s %15s %u %u",
                       dss, file_name, &stripe, &disk_idx) != 4) {
                LOG_INFO("Failed to parse READ header: %s", buffer);
                continue;
            }

            // construct file path for the requested block
            char file_path[512];
            // DATA block
            snprintf(file_path, sizeof(file_path), "storage/%s/%s/%s_stripe%u_disk%u_DATA.bin",
                     DISK_NAME, dss, file_name, stripe, disk_idx);
            
            FILE *fp = fopen(file_path, "rb");
            if (!fp) {
                // PARITY block
                snprintf(file_path, sizeof(file_path), "storage/%s/%s/%s_stripe%u_disk%u_PARITY.bin",
                         DISK_NAME, dss, file_name, stripe, disk_idx);
                fp = fopen(file_path, "rb");
            }

            char response_header[256];
            if (fp) {
                // get file size
                fseek(fp, 0, SEEK_END);
                long file_size = ftell(fp);
                fseek(fp, 0, SEEK_SET);

                if (file_size > 0) {
                    unsigned char *block_data = (unsigned char *)malloc(file_size);
                    if (block_data && fread(block_data, 1, file_size, fp) == (size_t)file_size) {
                        int header_len = snprintf(response_header, sizeof(response_header),
                                                  "READ-RESPONSE SUCCESS\n");
                        
                        size_t total_size = header_len + file_size;
                        unsigned char *response = (unsigned char *)malloc(total_size);
                        if (response) {
                            memcpy(response, response_header, header_len);
                            memcpy(response + header_len, block_data, file_size);
                            
                            sendto(COPY_SOCK, response, total_size, 0,
                                   (struct sockaddr *)&src_addr, src_len);
                            free(response);
                        }
                    }
                    free(block_data);
                }
                fclose(fp);
            } else {
                snprintf(response_header, sizeof(response_header), "READ-RESPONSE FAILURE\n");
                sendto(COPY_SOCK, response_header, strlen(response_header), 0,
                       (struct sockaddr *)&src_addr, src_len);
            }
        }
        else {
            LOG_INFO("Unknown command received: %s", buffer);
        }
    }

    free(buffer);
    return NULL;
}

int receive_cmd(char *cmd_buffer, char *local_ip)
{
    printf("\nAvailable commands:\n");
    printf("  register-disk\n");
    printf("  deregister-disk\n");
    printf("  exit\n\n");
    printf("disk-%s> ", DISK_NAME);

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
    
    if (strcmp(line, "register-disk") == 0) {
        snprintf(cmd_buffer, COMMAND_SIZE, "register-disk %s %s %hu %hu",
                 DISK_NAME, local_ip, MPORT, CPORT);
    } 
    else if (strcmp(line, "deregister-disk") == 0) {
        snprintf(cmd_buffer, COMMAND_SIZE, "deregister-disk %s", DISK_NAME);
    } 
    else {
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
        fprintf(stderr, "Usage: %s <DISK NAME> <MANAGER IPv4 ADDRESS> " \
                         "<MANAGER PORT> <MANAGEMENT PORT NUMBER> <COMMAND PORT>\n", argv[0]);
        exit(1);
    }


    char* disk_name = argv[1];
    char* manager_ip = argv[2];
    uint16_t manager_port = atoi(argv[3]);
    MPORT = atoi(argv[4]);
    CPORT = atoi(argv[5]);
    strncpy(DISK_NAME, disk_name, DISK_NAME_LEN - 1);
    DISK_NAME[DISK_NAME_LEN - 1] = '\0';

    // construct manager sockaddr_in structure
    memset(&MANAGER_ADDR, 0, sizeof(MANAGER_ADDR));
    MANAGER_ADDR.sin_family = AF_INET;
    MANAGER_ADDR.sin_addr.s_addr = inet_addr(manager_ip);
    MANAGER_ADDR.sin_port = htons(manager_port);

    char local_ip[16];
    get_local_ip(local_ip, sizeof(local_ip));
    LOG_INFO("[disk name]: %s [local IP]: %s", DISK_NAME, local_ip);

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

    if (pthread_create(&COPY_THREAD, NULL, copy_listener, NULL) != 0) {
        LOG_ERROR("Failed to launch copy listener thread");
    } else {
        COPY_THREAD_STARTED = 1;
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
        uint src_len = sizeof(src_addr);
        char response[RESPONSE_SIZE];
        ssize_t resp_len;

        if ( (resp_len = recvfrom(MANAGER_SOCK, response, RESPONSE_SIZE-1, 0, (struct sockaddr*)&src_addr, &src_len)) < 0) {
            LOG_ERROR("failed to receive response");
        }

        response[resp_len] = '\0';

        // stupid check which is technically very easy to circumvent from attacker's POV
        // TODO: make this more robust
        if (MANAGER_ADDR.sin_addr.s_addr != src_addr.sin_addr.s_addr) {
            LOG_ERROR("Received response from unknown source. MITM possible. Halting");
        }

        printf("[response]: %s [src ip]: %s\n", response, inet_ntoa(src_addr.sin_addr));
    }

    LOG_INFO("Closing sockets");
    if (MANAGER_SOCK >= 0) {
        close(MANAGER_SOCK);
    }
    COPY_THREAD_ACTIVE = 0;
    if (COPY_SOCK >= 0) {
        if (COPY_THREAD_STARTED) {
            int poke_sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (poke_sock >= 0) {
                struct sockaddr_in poke = {0};
                poke.sin_family = AF_INET;
                poke.sin_port = htons(CPORT);
                poke.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                sendto(poke_sock, "STOP", 4, 0, (struct sockaddr*)&poke, sizeof(poke));
                close(poke_sock);
            }
            pthread_join(COPY_THREAD, NULL);
        }
        close(COPY_SOCK);
    }
    LOG_INFO("Exiting");
    exit(0);
}
