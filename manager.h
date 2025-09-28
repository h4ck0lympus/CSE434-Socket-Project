#ifndef MANAGER_H
#define MANAGER_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

/*
    single threaded manager process to support management 
    of the processes implementing k >= 1 DSS
*/

#define COMMAND_SIZE 32

// command handler that will handle the incoming commands
// return 0 is success else -1
int command_handler(char* command);

// register's user inside manager's state
// return 0 is success else -1
int register_user();

#endif // MANAGER_H
