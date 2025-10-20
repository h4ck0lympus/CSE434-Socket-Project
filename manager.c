#include "manager.h"
#include "logging.h"
#include <string.h>

static User users[MAX_PEERS];
static int user_count = 0;

static Disk disks[MAX_PEERS];
static int disk_count = 0;

static DSS dss_list[MAX_DSS];
static int dss_count = 0;

static int SOCK;
struct sockaddr_in MANAGER_ADDR;
static uint16_t MANAGER_PORT;
static int g_rng_seeded = 0;

static PendingCopy pending_copy = {0};
static PendingRead pending_read = {0};
static PendingDiskFailure pending_failure = {0};
static PendingDecommission pending_decommission = {0};

static int is_power_of_two(uint32_t n)
{
    return n && !(n & (n - 1));
}

static int port_in_use(uint16_t p)
{
    for (int i = 0; i < user_count; i++)
        if (users[i].mport == p || users[i].cport == p) return 1;
    for (int i = 0; i < disk_count; i++)
        if (disks[i].mport == p || disks[i].cport == p) return 1;
    return 0;
}

static int valid_ports(uint16_t m, uint16_t c)
{
    if (m == c) return 0;
    return !port_in_use(m) && !port_in_use(c);
}

static void init_rng(void) {
    if (!g_rng_seeded) {
        unsigned seed = (unsigned)time(NULL) ^ (unsigned)getpid();
        srand(seed);
        g_rng_seeded = 1;
    }
}

int register_user(char* name, char* ip, uint16_t mport, uint16_t cport)
{
    if (user_count >= MAX_PEERS) { 
        LOG_WARN("Too many users");
        return -1; 
    }

    if (!valid_ports(mport, cport)) { 
        LOG_WARN("Port conflict or mport==cport"); 
        return -1; 
    }

    for (int i=0; i < user_count; i++) {
        if (strcmp(users[i].name, name) == 0) {
            LOG_WARN("User %s already registered", name);
            return -1;
        }

        if (users[i].mport == mport || users[i].cport == cport) {
            LOG_WARN("Port already in use");
            return -1;
        }
    }

    strncpy(users[user_count].name, name, NAME_LEN - 1);
    strncpy(users[user_count].ipv4_addr, ip, 15);
    users[user_count].mport = mport;
    users[user_count].cport = cport;
    user_count++;

    LOG_INFO("Registered user: %s at %s:%hu", name, ip, mport);
    return 0;
}

int register_disk(char* name, char* ip, uint16_t mport, uint16_t cport)
{
    if (disk_count >= MAX_PEERS) { 
        LOG_WARN("Too many disks"); 
        return -1; 
    }

    if (!valid_ports(mport, cport)) {
        LOG_WARN("Port conflict or mport==cport"); 
        return -1; 
    }

    for (int i = 0; i < disk_count; i++) {
        if (strcmp(disks[i].name, name) == 0) {
            LOG_WARN("Disk %s already registered", name);
            return -1;
        }
        if (disks[i].mport == mport || disks[i].cport == cport) {
            LOG_WARN("Port already in use");
            return -1;
        }
    }
    
    strncpy(disks[disk_count].name, name, NAME_LEN - 1);
    strncpy(disks[disk_count].ipv4_addr, ip, 15);
    disks[disk_count].mport = mport;
    disks[disk_count].cport = cport;
    disks[disk_count].state = FREE;
    memset(disks[disk_count].dss_name, 0, NAME_LEN);
    disk_count++;
    
    LOG_INFO("Registered disk: %s at %s:%hu", name, ip, mport);
    return 0;
}

int configure_dss(char *dss_name, int n, uint32_t striping_unit) {
    // basic checks
    if (n < 3) {
        LOG_WARN("DSS configuration failed: n must be >= 3");
        return -1;
    }
    if (!is_power_of_two(striping_unit) || striping_unit < 128 || striping_unit > 1048576) {
        LOG_WARN("DSS configuration failed: invalid striping unit");
        return -1;
    }
    if (dss_count >= MAX_DSS) {
        LOG_WARN("DSS configuration failed: max DSS reached");
        return -1;
    }
    for (int i = 0; i < dss_count; i++) {
        if (strcmp(dss_list[i].name, dss_name) == 0) {
            LOG_WARN("DSS %s already exists", dss_name);
            return -1;
        }
    }

    int free_idx[MAX_PEERS];
    int free_count = 0;
    for (int i = 0; i < disk_count; i++) {
        if (disks[i].state == FREE) {
            free_idx[free_count++] = i;
        }
    }
    if (free_count < n) {
        LOG_WARN("Insufficient free disks: need %d, have %d", n, free_count);
        return -1;
    }

    for (int i = free_count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int t = free_idx[i];
        free_idx[i] = free_idx[j];
        free_idx[j] = t;
    }

    strncpy(dss_list[dss_count].name, dss_name, NAME_LEN - 1);
    dss_list[dss_count].name[NAME_LEN - 1] = '\0';
    dss_list[dss_count].n = n;
    dss_list[dss_count].striping_unit = striping_unit;
    dss_list[dss_count].file_count = 0;

    for (int k = 0; k < n; k++) {
        int di = free_idx[k];
        disks[di].state = IN_DSS;
        strncpy(disks[di].dss_name, dss_name, NAME_LEN - 1);
        disks[di].dss_name[NAME_LEN - 1] = '\0';
        dss_list[dss_count].disk_indices[k] = di;
    }

    LOG_INFO("Configured DSS %s with n=%d, striping_unit=%u",
             dss_name, n, striping_unit);
    printf("Assigned disks: ");
    for (int i = 0; i < n; i++) {
        printf("%s ", disks[dss_list[dss_count].disk_indices[i]].name);
    }
    printf("\n");

    dss_count++;
    return 0;
}

int deregister_user(char *name) 
{
    for (int i = 0; i < user_count; i++) {
        if (strcmp(users[i].name, name) == 0) {
            for (int j = i; j < user_count - 1; j++) {
                users[j] = users[j + 1];
            }
            user_count--;
            LOG_INFO("Deregistered user: %s", name);
            return 0;
        }
    }
    LOG_WARN("User %s not found", name);
    return -1;
}

int deregister_disk(char *name)
{
    for (int i = 0; i < disk_count; i++) {
        if (strcmp(disks[i].name, name) == 0) {
            if (disks[i].state == IN_DSS) {
                LOG_WARN("Disk %s is in DSS %s, cannot deregister", 
                        name, disks[i].dss_name);
                return -1;
            }
            for (int j = i; j < disk_count - 1; j++) {
                disks[j] = disks[j + 1];
            }
            disk_count--;
            LOG_INFO("Deregistered disk: %s", name);
            return 0;
        }
    }
    LOG_WARN("Disk %s not found", name);
    return -1;
}

int ls(char* response_buffer, size_t buffer_size)
{
    if (dss_count == 0) {
        return -1;
    }

    int offset = 0;
    for (int i = 0; i < dss_count; i++) {
        DSS *dss = &dss_list[i];
        
        offset += snprintf(response_buffer + offset, buffer_size - offset,
                          "%s: Disk array with n=%d (", dss->name, dss->n);
        
        for (int j = 0; j < dss->n; j++) {
            int disk_idx = dss->disk_indices[j];
            offset += snprintf(response_buffer + offset, buffer_size - offset,
                              "%s%s", disks[disk_idx].name, 
                              (j < dss->n - 1) ? ", " : "");
        }
        
        offset += snprintf(response_buffer + offset, buffer_size - offset,
                          ") with striping-unit %u B.\n", dss->striping_unit);
        
        for (int k = 0; k < dss->file_count; k++) {
            File *file = &dss->files[k];
            offset += snprintf(response_buffer + offset, buffer_size - offset,
                              "%s %lu B %s\n", file->name, file->size, file->owner);
        }
        
        if (i < dss_count - 1) {
            offset += snprintf(response_buffer + offset, buffer_size - offset, "\n");
        }
    }
    
    return 0;
}

int copy(char* file_name, int file_size, char* owner) {
    if (dss_count == 0) {
        LOG_WARN("No DSSs configured for copy operation");
        return -1;
    }

    int selected_dss_idx = -1;
    for (int i=0; i < dss_count; i++) {
        if (dss_list[i].file_count < MAX_FILES) {
            selected_dss_idx = i;
            break;
        }
    }

    if (selected_dss_idx < 0) {
        LOG_WARN("No DSS is empty for the file");
        return -2;
    }

    strncpy(pending_copy.file_name, file_name, NAME_LEN - 1);
    pending_copy.file_name[NAME_LEN - 1] = '\0';
    pending_copy.file_size = file_size;
    strncpy(pending_copy.owner, owner, NAME_LEN - 1);
    pending_copy.owner[NAME_LEN - 1] = '\0';
    pending_copy.dss_idx = selected_dss_idx;
    pending_copy.is_pending = 1;

    DSS* selected_dss = &dss_list[selected_dss_idx];
    // phase-1 ended now phase 2 will occur from user's side
    LOG_INFO("Phase 1: Selected DSS %s for file %s (%d bytes) from owner %s", 
             selected_dss->name, file_name, file_size, owner);
    return selected_dss_idx;
}

int read_file(char* file_name, char* user_name) {
    if (dss_count == 0) {
        LOG_WARN("No DSSs configured for read operation");
        return -1;
    }

    for (int i = 0; i < dss_count; i++) {
        DSS *dss = &dss_list[i];
        for (int j = 0; j < dss->file_count; j++) {
            if (strcmp(dss->files[j].name, file_name) == 0) {
                pending_read.active = 1;
                pending_read.dss_idx = i;
                strncpy(pending_read.file, file_name, NAME_LEN - 1);
                pending_read.file[NAME_LEN - 1] = '\0';
                strncpy(pending_read.user, user_name, NAME_LEN - 1);
                pending_read.user[NAME_LEN - 1] = '\0';
                pending_read.size = dss->files[j].size;
                
                LOG_INFO("Phase 1: Found file %s in DSS %s (%zu bytes) for user %s", 
                         file_name, dss->name, pending_read.size, user_name);
                return i;
            }
        }
    }
    
    LOG_WARN("File %s not found in any DSS", file_name);
    return -2;
}

int disk_failure(char* dss_name) {
    if (dss_count == 0) {
        LOG_WARN("No DSSs configured for disk-failure operation");
        return -1;
    }

    if (pending_read.active) {
        LOG_WARN("Read operation in progress, disk-failure not allowed");
        return -2;
    }

    int dss_idx = -1;
    for (int i = 0; i < dss_count; i++) {
        if (strcmp(dss_list[i].name, dss_name) == 0) {
            dss_idx = i;
            break;
        }
    }

    if (dss_idx < 0) {
        LOG_WARN("DSS %s not found", dss_name);
        return -3;
    }

    strncpy(pending_failure.dss_name, dss_name, NAME_LEN - 1);
    pending_failure.dss_name[NAME_LEN - 1] = '\0';
    pending_failure.dss_idx = dss_idx;
    pending_failure.is_pending = 1;

    LOG_INFO("Phase 1: Disk failure initiated for DSS %s", dss_name);
    return dss_idx;
}

int decommission_dss(char* dss_name) {
    if (dss_count == 0) {
        LOG_WARN("No DSSs configured for decommission operation");
        return -1;
    }

    int dss_idx = -1;
    for (int i = 0; i < dss_count; i++) {
        if (strcmp(dss_list[i].name, dss_name) == 0) {
            dss_idx = i;
            break;
        }
    }

    if (dss_idx < 0) {
        LOG_WARN("DSS %s not found", dss_name);
        return -2;
    }

    strncpy(pending_decommission.dss_name, dss_name, NAME_LEN - 1);
    pending_decommission.dss_name[NAME_LEN - 1] = '\0';
    pending_decommission.dss_idx = dss_idx;
    pending_decommission.is_pending = 1;

    LOG_INFO("Decommission initiated for DSS %s (critical section active)", dss_name);
    return dss_idx;
}

static int decommission_response(int dss_idx, char* out, size_t outlen)
{
    DSS* dss = &dss_list[dss_idx];
    size_t off = 0;

    int w = snprintf(out + off, outlen - off, "DECOMMISSION-PARAMS %s %d %u",
                     dss->name, dss->n, dss->striping_unit);

    if (w < 0 || (size_t)w >= outlen - off)
        return -1;
    off += w;

    for (int i=0; i < dss->n; i++) {
        int di = dss->disk_indices[i];
        w = snprintf(out + off, outlen-off, " %s %s %hu",
                 disks[di].name, disks[di].ipv4_addr, disks[di].cport);
        if (w < 0 || (size_t)w >= outlen - off)
            return -1;
        off+=w;
    }
    return 0;
}

static int failure_response(int dss_idx, char* out, size_t outlen)
{
    DSS* dss = &dss_list[dss_idx];
    size_t off = 0;

    int w = snprintf(out + off, outlen - off, "FAILURE-PARAMS %s %d %u",
                     dss->name, dss->n, dss->striping_unit);

    if (w < 0 || (size_t)w >= outlen - off)
        return -1;
    off += w;

    for (int i=0; i < dss->n; i++) {
        int di = dss->disk_indices[i];
        w = snprintf(out + off, outlen-off, " %s %s %hu",
                 disks[di].name, disks[di].ipv4_addr, disks[di].cport);
        if (w < 0 || (size_t)w >= outlen - off)
            return -1;
        off+=w;
    }
    return 0;
}

static int read_response(int dss_idx, char* out, size_t outlen)
{
    DSS* dss = &dss_list[dss_idx];
    size_t off = 0;

    int w = snprintf(out + off, outlen - off, "READ-PARAMS %s %d %u %zu",
                     dss->name, dss->n, dss->striping_unit, pending_read.size);

    if (w < 0 || (size_t)w >= outlen - off)
        return -1;
    off += w;

    for (int i=0; i < dss->n; i++) {
        int di = dss->disk_indices[i];
        w = snprintf(out + off, outlen-off, " %s %s %hu",
                 disks[di].name, disks[di].ipv4_addr, disks[di].cport);
        if (w < 0 || (size_t)w >= outlen - off)
            return -1;
        off+=w;
    }
    return 0;
}

static int copy_response(int dss_idx, char* out, size_t outlen)
{
    DSS* dss = &dss_list[dss_idx];
    size_t off = 0;

    int w = snprintf(out + off, outlen - off, "COPY-PARAMS %s %d %u",
                     dss->name, dss->n, dss->striping_unit);

    // very rare case but still good to consider...
    if (w < 0 || (size_t)w >= outlen - off)
        return -1;
    off += w;

    for (int i=0; i < dss->n; i++) {
        int di = dss->disk_indices[i];
        w = snprintf(out + off, outlen-off, " %s %s %hu",
                 disks[di].name, disks[di].ipv4_addr, disks[di].cport);
        if (w < 0 || (size_t)w >= outlen - off)
            return -1;
        off+=w;
    }
    return 0;
}

int command_handler(char *cmd, struct sockaddr_in* client_addr, socklen_t client_len)
{
    char response[4096];
    
    // check if copy operation is pending, if it is then copy-complete is only allowed
    if (pending_copy.is_pending && strncmp(cmd, "copy-complete", 13) != 0) {
        strcpy(response, "FAILURE: Copy operation in progress, only copy-complete allowed");
        goto send_response;
    }
    
    // check if read operation is pending, if it is then read-complete is only allowed
    if (pending_read.active && strncmp(cmd, "read-complete", 13) != 0) {
        strcpy(response, "FAILURE: Read operation in progress, only read-complete allowed");
        goto send_response;
    }
    
    // check if disk failure operation is pending, if it is then recovery-complete is only allowed
    if (pending_failure.is_pending && strncmp(cmd, "recovery-complete", 17) != 0) {
        strcpy(response, "FAILURE: Disk failure operation in progress, only recovery-complete allowed");
        goto send_response;
    }
    
    // check if decommission operation is pending, block ALL commands except decommission-complete
    if (pending_decommission.is_pending && strncmp(cmd, "decommission-complete", 21) != 0) {
        strcpy(response, "FAILURE: Decommission operation in progress (critical section)");
        goto send_response;
    }
    
    if (strncmp(cmd, "register-user", 13) == 0) {
        char name[NAME_LEN], ip[16];
        uint16_t mport, cport;
        if (sscanf(cmd, "register-user %s %s %hu %hu", \
                   name, ip, &mport, &cport) == 4) {
            if (register_user(name, ip, mport, cport) == 0) {
                strcpy(response, "SUCCESS: User registered");
            } else {
                strcpy(response, "FAILURE: Unable to register user");
            }
        } else {
            strcpy(response, "FAILURE: failed to read arguments. Incorrect command format");
        }
    } else if (strncmp(cmd, "register-disk", 13) == 0) {
        char name[NAME_LEN], ip[16];
        uint16_t mport, cport;
        if (sscanf(cmd, "register-disk %s %s %hu %hu", \
                   name, ip, &mport, &cport) == 4) {
            if (register_disk(name, ip, mport, cport) == 0) {
                strcpy(response, "SUCCESS: Disk registered");
            } else {
                strcpy(response, "FAILURE: Unable to register disk");
            }
        } else {
            strcpy(response, "FAILURE: failed to read arguments. Incorrect command format");
        }
    } else if (strncmp(cmd, "configure-dss", 13) == 0) {
        char dss_name[NAME_LEN];
        int n;
        uint32_t striping_unit;
        if (sscanf(cmd, "configure-dss %s %d %u", 
                  dss_name, &n, &striping_unit) == 3) {
            if (configure_dss(dss_name, n, striping_unit) == 0) {
                strcpy(response, "SUCCESS: Dss configured");
            } else {
                strcpy(response, "FAILURE: Unable to configure dss");
            }
        } else {
            strcpy(response, "FAILURE: failed to read arguments. Incorrect command format");
        }
    } else if (strncmp(cmd, "deregister-user", 15) == 0) {
        char name[NAME_LEN];
        if (sscanf(cmd, "deregister-user %s", name) == 1) {
            if (deregister_user(name) == 0) {
                strcpy(response, "SUCCESS: Deregistered User");
            } else {
                strcpy(response, "FAILURE: Unable to deregister user");
            }
        } else {
            strcpy(response, "FAILURE: failed to read arguments. Incorrect command format");
        }
    } else if (strncmp(cmd, "deregister-disk", 15) == 0) {
        char name[NAME_LEN];
        if (sscanf(cmd, "deregister-disk %s", name) == 1) {
            if (deregister_disk(name) == 0) {
                strcpy(response, "SUCCESS: Deregistered Disk");
            } else {
                strcpy(response, "FAILURE: Unable to deregister disk");
            }
        } else {
            strcpy(response, "FAILURE: failed to read arguments. Incorrect command format");
        }
    } else if (strncmp(cmd, "ls", 2) == 0) {
        char temp[4096];
        if (ls(temp, sizeof(temp)) == 0) {
            const char *prefix = "SUCCESS\n";
            size_t prefix_len = strlen(prefix);
            size_t temp_len = strnlen(temp, sizeof(temp));
            size_t max_temp = sizeof(response) - prefix_len - 1;
            if (temp_len > max_temp) {
                temp_len = max_temp;
            }
            memcpy(response, prefix, prefix_len);
            memcpy(response + prefix_len, temp, temp_len);
            response[prefix_len + temp_len] = '\0';
        } else {
            strcpy(response, "FAILURE: No DSSs configured");
        }
    } else if (strncmp(cmd, "copy ", 5) == 0) {
        char file_name[NAME_LEN];
        int file_size;
        char owner[NAME_LEN];
        if (sscanf(cmd, "copy %s %d %s", file_name, &file_size, owner) == 3) {
            int resp_code = copy(file_name, file_size, owner);
            if (resp_code >= 0) {
                if (copy_response(resp_code, response, sizeof(response)) != 0) {
                    pending_copy.is_pending = 0; // flip the switch
                    strcpy(response, "FAILURE: Response too large");
                }
            } else if (resp_code == -1) {
                strcpy(response, "FAILURE: No DSSs configured");
            } else if (resp_code == -2) {
                strcpy(response, "FAILURE: No DSSs empty.");
            }
        } else {
            strcpy(response, "FAILURE: failed to read arguments. Incorrect command format");
        }
    } else if (strncmp(cmd, "copy-complete", 13) == 0) {
        if (!pending_copy.is_pending) {
            strcpy(response, "FAILURE: No pending copy operation");
        } else {
            DSS *dss = &dss_list[pending_copy.dss_idx];
            File *new_file = &dss->files[dss->file_count];
            snprintf(new_file->name, NAME_LEN, "%s", pending_copy.file_name);
            new_file->size = pending_copy.file_size;
            snprintf(new_file->owner, NAME_LEN, "%s", pending_copy.owner);
            dss->file_count++;
            
            LOG_INFO("Phase 2: File %s successfully added to DSS %s", 
                     pending_copy.file_name, dss->name);
            
            pending_copy.is_pending = 0;
            strcpy(response, "SUCCESS: copy-complete");
        }
    } else if (strncmp(cmd, "read ", 5) == 0) {
        char dss_name[NAME_LEN], file_name[NAME_LEN], user_name[NAME_LEN];
        if (sscanf(cmd, "read %255s %255s %255s", dss_name, file_name, user_name) == 3) {
            // find dss
            int dss_idx = -1;
            for (int i = 0; i < dss_count; i++) {
                if (strcmp(dss_list[i].name, dss_name) == 0) {
                    dss_idx = i;
                    break;
                }
            }
            if (dss_idx < 0) {
                strcpy(response, "FAILURE: DSS not found");
                goto send_response;
            }
            // find file in dss and check owner
            DSS *dss = &dss_list[dss_idx];
            int file_found = 0;
            for (int j = 0; j < dss->file_count; j++) {
                if (strcmp(dss->files[j].name, file_name) == 0 && strcmp(dss->files[j].owner, user_name) == 0) {
                    file_found = 1;
                    pending_read.size = dss->files[j].size;
                    break;
                }
            }
            if (!file_found) {
                strcpy(response, "FAILURE: File not found or access denied");
                goto send_response;
            }
            // set pending
            pending_read.active = 1;
            pending_read.dss_idx = dss_idx;
            strncpy(pending_read.file, file_name, NAME_LEN - 1);
            pending_read.file[NAME_LEN - 1] = '\0';
            strncpy(pending_read.user, user_name, NAME_LEN - 1);
            pending_read.user[NAME_LEN - 1] = '\0';
            if (read_response(dss_idx, response, sizeof(response)) != 0) {
                pending_read.active = 0;
                strcpy(response, "FAILURE: Response too large");
            }
        } else {
            strcpy(response, "FAILURE: failed to read arguments. Incorrect command format");
        }
    } else if (strncmp(cmd, "read-complete", 13) == 0) {
        if (!pending_read.active) {
            strcpy(response, "FAILURE: No pending read operation");
        } else {
            LOG_INFO("Phase 2: File %s successfully read by user %s", 
                     pending_read.file, pending_read.user);
            
            pending_read.active = 0;
            strcpy(response, "SUCCESS: read-complete");
        }
    } else if (strncmp(cmd, "disk-failure ", 13) == 0) {
        char dss_name[NAME_LEN];
        if (sscanf(cmd, "disk-failure %s", dss_name) == 1) {
            int resp_code = disk_failure(dss_name);
            if (resp_code >= 0) {
                if (failure_response(resp_code, response, sizeof(response)) != 0) {
                    pending_failure.is_pending = 0;
                    strcpy(response, "FAILURE: Response too large");
                }
            } else if (resp_code == -1) {
                strcpy(response, "FAILURE: No DSSs configured");
            } else if (resp_code == -2) {
                strcpy(response, "FAILURE: Read operation in progress");
            } else if (resp_code == -3) {
                strcpy(response, "FAILURE: DSS not found");
            }
        } else {
            strcpy(response, "FAILURE: failed to read arguments. Incorrect command format");
        }
    } else if (strncmp(cmd, "recovery-complete", 17) == 0) {
        if (!pending_failure.is_pending) {
            strcpy(response, "FAILURE: No pending disk failure operation");
        } else {
            LOG_INFO("Phase 2: Disk failure recovery completed for DSS %s", 
                     pending_failure.dss_name);
            
            pending_failure.is_pending = 0;
            strcpy(response, "SUCCESS: recovery-complete");
        }
    } else if (strncmp(cmd, "decommission-dss ", 17) == 0) {
        char dss_name[NAME_LEN];
        if (sscanf(cmd, "decommission-dss %s", dss_name) == 1) {
            int resp_code = decommission_dss(dss_name);
            if (resp_code >= 0) {
                if (decommission_response(resp_code, response, sizeof(response)) != 0) {
                    pending_decommission.is_pending = 0;
                    strcpy(response, "FAILURE: Response too large");
                }
            } else if (resp_code == -1) {
                strcpy(response, "FAILURE: No DSSs configured");
            } else if (resp_code == -2) {
                strcpy(response, "FAILURE: DSS not found");
            }
        } else {
            strcpy(response, "FAILURE: failed to read arguments. Incorrect command format");
        }
    } else if (strncmp(cmd, "decommission-complete", 21) == 0) {
        if (!pending_decommission.is_pending) {
            strcpy(response, "FAILURE: No pending decommission operation");
        } else {
            // free all disks in this DSS (change state from IN_DSS to FREE)
            DSS *dss = &dss_list[pending_decommission.dss_idx];
            for (int i = 0; i < dss->n; i++) {
                int disk_idx = dss->disk_indices[i];
                disks[disk_idx].state = FREE;
                memset(disks[disk_idx].dss_name, 0, NAME_LEN);
                LOG_INFO("Disk %s changed from IN_DSS to FREE", disks[disk_idx].name);
            }
            
            // remove DSS from manager state
            for (int i = pending_decommission.dss_idx; i < dss_count - 1; i++) {
                dss_list[i] = dss_list[i + 1];
            }
            dss_count--;
            
            LOG_INFO("DSS %s decommissioned and removed from manager state", 
                     pending_decommission.dss_name);
            
            pending_decommission.is_pending = 0;
            strcpy(response, "SUCCESS: decommission-complete");
        }
    } else {
        LOG_WARN("Unknown command: %s", cmd);
    }

send_response:
    LOG_INFO("[SENDING] %s", response);
    sendto(SOCK, response, strlen(response), 0,
           (struct sockaddr*)client_addr, client_len);

    return 0;
}

int main(int argc, char* argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <MANAGER SERVER PORT>\n", argv[0]);
        exit(1);
    }

    MANAGER_PORT = atoi(argv[1]);

    // make a UDP socket
    if ( (SOCK = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0 ) {
        LOG_ERROR("socket() failed");
    }

    // construct local address structure
    memset(&MANAGER_ADDR, 0, sizeof(MANAGER_ADDR));
    MANAGER_ADDR.sin_family = AF_INET;
    MANAGER_ADDR.sin_addr.s_addr = htonl(INADDR_ANY);
    MANAGER_ADDR.sin_port = htons(MANAGER_PORT);

    // bind to local address
    if ( (bind(SOCK, (struct sockaddr*) &MANAGER_ADDR, sizeof(MANAGER_ADDR)) < 0) )  {
        LOG_ERROR("bind() failed");
    }

    LOG_INFO("Manager serving on port %hu", MANAGER_PORT);
    init_rng();
    while (1) {
        char cmd[COMMAND_SIZE];
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        ssize_t client_msg_size = recvfrom(SOCK, cmd, COMMAND_SIZE - 1, 0, (struct sockaddr*)&client_addr , &client_len);

        if ( client_msg_size < 0 ) {
            LOG_ERROR("recvfrom() failed");
        }
        cmd[client_msg_size] = '\0';
        LOG_INFO("[cmd]: %s (received from %s)", cmd, inet_ntoa(client_addr.sin_addr));

        command_handler(cmd, &client_addr, client_len);

        printf("\n=== Manager State ===\n");
        printf("Users: %d, Disks: %d, DSSs: %d\n", user_count, disk_count, dss_count);
        printf("Free disks: ");
        for (int i = 0; i < disk_count; i++) {
            if (disks[i].state == FREE) printf("%s ", disks[i].name);
        }
        printf("\n\n");
    }
}
