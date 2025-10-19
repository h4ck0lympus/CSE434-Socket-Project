#include "user.h"
#include <stdio.h>
#include <string.h>

static char USERNAME[USERNAME_LEN];
static struct sockaddr_in MANAGER_ADDR;
static uint16_t MPORT;
static uint16_t CPORT;

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

int receive_cmd(char *cmd_buffer, char *local_ip)
{
    printf("\nAvailable commands:\n");
    printf("  register-user\n");
    printf("  deregister-user\n");
    printf("  configure-dss <dss-name> <n> <striping-unit>\n");
    printf("  ls");
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
        uint src_len = sizeof(src_addr);
        char response[RESPONSE_SIZE];
        ssize_t resp_len;

        if ( (resp_len = recvfrom(sock, response, RESPONSE_SIZE-1, 0, (struct sockaddr*)&src_addr, &src_len)) < 0) {
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

    LOG_INFO("Closing socket");
    close(sock);
    LOG_INFO("Exiting");
    exit(0);
}
