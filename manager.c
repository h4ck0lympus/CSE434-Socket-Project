#include "manager.h"
#include "logging.h"

struct sockaddr_in manager_addr;
struct sockaddr_in client_addr;

static uint16_t MANAGER_PORT;

// TODO
int command_handler(char *command)
{
    (void)command;
    return 0;
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
