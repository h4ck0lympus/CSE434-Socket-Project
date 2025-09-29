#include "manager.h"
#include "logging.h"

static User users[MAX_PEERS];
static int user_count = 0;

static Disk disks[MAX_PEERS];
static int disk_count = 0;

static DSS dss_list[MAX_DSS];
static int dss_count = 0;

struct sockaddr_in manager_addr;
struct sockaddr_in client_addr;
static uint16_t MANAGER_PORT;

int register_user(char* name, char* ip, uint16_t mport, uint16_t cport)
{
    return 0; // SUCCESS
}

int register_disk(char* name, char* ip, uint16_t mport, uint16_t cport)
{
    return 0;
}

int configure_dss(char* dss_name, int n, uint32_t stripping_unit)
{
    return 0;
}

// TODO
int command_handler(char *command)
{
    if (strcmp(command, "register-user") == 0) {
        return register_user();
    } else if (strcmp(command, "register-disk") == 0) {
        return register_disk();
    } else {
        LOG_INFO("Unknown command recieved");
        return -1;
    }
}

int main(int argc, char* argv[])
{
    int sock;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <MANAGER SERVER PORT>\n", argv[0]);
        exit(1);
    }

    MANAGER_PORT = atoi(argv[1]);

    // make a UDP socket
    if ( (sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0 ) {
        LOG_ERROR("socket() failed");
    }

    // construct local address structure
    memset(&manager_addr, 0, sizeof(manager_addr));
    manager_addr.sin_family = AF_INET;
    manager_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    manager_addr.sin_port = htons(MANAGER_PORT);

    // bind to local address
    if ( (bind(sock, (struct sockaddr*) &manager_addr, sizeof(manager_addr)) < 0) )  {
        LOG_ERROR("bind() failed");
    }

    while (1) {
        char cmd[COMMAND_SIZE];
        socklen_t client_len = sizeof(client_addr);
        ssize_t client_msg_size = recvfrom(sock, cmd, COMMAND_SIZE - 1, 0, (struct sockaddr*)&client_addr , &client_len);

        if ( client_msg_size < 0 ) {
            LOG_ERROR("recvfrom() failed");
        }
        cmd[client_msg_size] = '\0';

        LOG_INFO("[cmd]: %s (received from %s)", cmd, inet_ntoa(client_addr.sin_addr));

        if (command_handler(cmd) != 0) {
            LOG_ERROR("command: %s failed. Manager exiting...", cmd);
        }
    }
}
