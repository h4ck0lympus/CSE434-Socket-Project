#include "user.h"
#include "logging.h"
#include "manager.h"

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
    if (sscanf(resp, "COPY-PARAMS %255s %d %u", dss, &n, &b) != 3) {
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

static void *read_block_thread(void *arg)
{
    BlockReadTask *task = (BlockReadTask *)arg;
    if (!task) return NULL;

    task->success = 0;
    *(task->block_data) = NULL;
    *(task->block_size) = 0;

    int s = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { LOG_WARN("read socket creation failed"); return NULL; }

    // short timeout so we don't hang forever
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    // bind to ephemeral port (0) on INADDR_ANY
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(0);
    bind(s, (struct sockaddr*)&local, sizeof local);

    // send request
    if (sendto(s, task->request, task->request_len, 0,
               (struct sockaddr *)&task->dest, sizeof(task->dest)) < 0) {
        LOG_WARN("sendto failed while requesting stripe block");
        close(s);
        return NULL;
    }

    // receive response
    const size_t buf_size = 2048 + 2 * 1024 * 1024;
    unsigned char *buffer = (unsigned char *)malloc(buf_size);
    if (!buffer) { LOG_WARN("Failed to allocate read buffer"); close(s); return NULL; }

    struct sockaddr_in src_addr; socklen_t src_len = sizeof(src_addr);
    ssize_t recv_len = recvfrom(s, buffer, buf_size, 0,
                                (struct sockaddr *)&src_addr, &src_len);
    if (recv_len < 0) {
        LOG_WARN("recvfrom failed while reading stripe block");
        free(buffer); close(s); return NULL;
    }

    // parse header + payload
    unsigned char *newline = memchr(buffer, '\n', (size_t)recv_len);
    if (!newline) { LOG_WARN("Malformed read response received"); free(buffer); close(s); return NULL; }

    size_t header_len = (size_t)(newline - buffer);
    size_t payload_len = (size_t)recv_len - header_len - 1;
    buffer[header_len] = '\0';

    char status[16];
    if (sscanf((char *)buffer, "READ-RESPONSE %15s", status) != 1) {
        LOG_WARN("Failed to parse READ response header");
        free(buffer); close(s); return NULL;
    }

    if (strcmp(status, "SUCCESS") == 0) {
        *(task->block_data) = (unsigned char *)malloc(payload_len);
        if (*(task->block_data)) {
            memcpy(*(task->block_data), newline + 1, payload_len);
            *(task->block_size) = payload_len;
            task->success = 1;
        }
    } else {
        LOG_WARN("Read failed for stripe %d disk %d", task->stripe_num, task->disk_idx);
    }

    free(buffer);
    close(s);
    return NULL;
}

static int read_params(char *resp)
{
    char dss[NAME_LEN];
    int n;
    unsigned b;
    size_t file_size;
    if (sscanf(resp, "READ-PARAMS %255s %d %u %zu", dss, &n, &b, &file_size) != 4) {
        LOG_WARN("FAILED: READ-PARAMS arguments not correct. response: %s", resp);
        return -1;
    }

    if (n <= 0) {
        LOG_WARN("Invalid disk count received in READ-PARAMS");
        return -1;
    }

    if (COPY_SOCK < 0) {
        LOG_WARN("Copy socket not initialized");
        return -1;
    }

    Peer peers[16];
    if (n > (int)(sizeof(peers) / sizeof(peers[0]))) {
        LOG_WARN("n is too big");
        return -1;
    }

    // parse disk information from READ-PARAMS response
    char *cursor = resp + 12; // skip READ-PARAMS
    for (int skip = 0; skip < 4 && *cursor; ) {
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

    unsigned int blocks_per_stripe = (unsigned int)n - 1;
    unsigned int stripe_bytes = blocks_per_stripe * b;
    unsigned int stripes = (file_size + stripe_bytes - 1) / stripe_bytes;

    if (stripes == 0) {
        stripes = 1;
    }

    // create output file
    char output_file[NAME_LEN + 4];
    snprintf(output_file, sizeof(output_file), "%s.out", LAST_COPY_FILE);
    FILE *fp = fopen(output_file, "wb");
    if (!fp) {
        LOG_WARN("Failed to create output file %s", output_file);
        return -1;
    }

    // read each stripe and reconstruct the file
    for (unsigned int s = 0; s < stripes; s++) {
        unsigned char *blocks[16];
        
        // initialize block storage
        for (int i = 0; i < n; i++) {
            blocks[i] = NULL;
        }

        // create threads to request blocks from each disk for this stripe
        pthread_t thread_ids[16];
        BlockReadTask *tasks[16];
        size_t block_sizes[16];
        
        for (int i = 0; i < n; i++) {
            block_sizes[i] = 0;
        }

        for (int d = 0; d < n; d++) {
            char request[256];
            int reqlen = snprintf(request, sizeof(request),
                                  "READ %s %s %u %u\n",
                                  dss, LAST_COPY_FILE, s, d);
            if (reqlen < 0 || (size_t)reqlen >= sizeof(request)) {
                LOG_WARN("Failed to construct read request");
                for (int j = 0; j < d; j++) {
                    pthread_join(thread_ids[j], NULL);
                    free(tasks[j]->request);
                    free(tasks[j]);
                }
                for (int j = 0; j < n; j++) {
                    free(blocks[j]);
                }
                fclose(fp);
                return -1;
            }

            BlockReadTask *task = (BlockReadTask *)malloc(sizeof(BlockReadTask));
            if (!task) {
                LOG_WARN("Failed to allocate read task");
                for (int j = 0; j < d; j++) {
                    pthread_join(thread_ids[j], NULL);
                    free(tasks[j]->request);
                    free(tasks[j]);
                }
                for (int j = 0; j < n; j++) {
                    free(blocks[j]);
                }
                fclose(fp);
                return -1;
            }

            task->dest = peers[d].addr;
            task->request = (char *)malloc(reqlen);
            if (!task->request) {
                LOG_WARN("Failed to allocate request buffer");
                free(task);
                for (int j = 0; j < d; j++) {
                    pthread_join(thread_ids[j], NULL);
                    free(tasks[j]->request);
                    free(tasks[j]);
                }
                for (int j = 0; j < n; j++) {
                    free(blocks[j]);
                }
                fclose(fp);
                return -1;
            }
            memcpy(task->request, request, reqlen);
            task->request_len = reqlen;
            task->block_data = &blocks[d];
            task->block_size = &block_sizes[d];
            task->stripe_num = s;
            task->disk_idx = d;
            tasks[d] = task;

            if (pthread_create(&thread_ids[d], NULL, read_block_thread, task) != 0) {
                LOG_WARN("Failed to spawn reader thread");
                free(task->request);
                free(task);
                for (int j = 0; j < d; j++) {
                    pthread_join(thread_ids[j], NULL);
                    free(tasks[j]->request);
                    free(tasks[j]);
                }
                for (int j = 0; j < n; j++) {
                    free(blocks[j]);
                }
                fclose(fp);
                return -1;
            }
        }

        // wait for all read threads to complete
        for (int t = 0; t < n; t++) {
            pthread_join(thread_ids[t], NULL);
            free(tasks[t]->request);
            free(tasks[t]);
        }

        // reconstruct stripe data
        unsigned char *stripe_data = (unsigned char *)malloc(stripe_bytes);
        if (!stripe_data) {
            LOG_WARN("Failed to allocate stripe buffer");
            for (int i = 0; i < n; i++) free(blocks[i]);
            fclose(fp);
            return -1;
        }

        unsigned int parity_disk = (unsigned int)n - ((s % (unsigned int)n) + 1);
        if (parity_disk >= (unsigned int)n) parity_disk = 0;

        // reconstruct data blocks
        unsigned int data_idx = 0;
        for (int d = 0; d < n; d++) {
            if ((unsigned int) d == parity_disk) continue;

            if (blocks[d]) {
                memcpy(stripe_data + data_idx * b, blocks[d], b);
            } else {
                // recover missing block using parity
                if (blocks[parity_disk]) {
                    unsigned char *recovered = (unsigned char *)malloc(b);
                    memcpy(recovered, blocks[parity_disk], b);
                    
                    for (int j = 0; j < n; j++) {
                        if (j != d && (unsigned int)j != parity_disk && blocks[j]) {
                            for (unsigned k = 0; k < b; k++) {
                                recovered[k] ^= blocks[j][k];
                            }
                        }
                    }
                    memcpy(stripe_data + data_idx * b, recovered, b);
                    free(recovered);
                } else {
                    memset(stripe_data + data_idx * b, 0, b);
                }
            }
            data_idx++;
        }

        // Write stripe to file
        size_t write_size = stripe_bytes;
        if (s == stripes - 1) {
            size_t remaining = file_size - s * stripe_bytes;
            if (remaining < stripe_bytes) {
                write_size = remaining;
            }
        }
        fwrite(stripe_data, 1, write_size, fp);

        // Cleanup
        for (int i = 0; i < n; i++) {
            free(blocks[i]);
        }
        free(stripe_data);
    }

    fclose(fp);
    LOG_INFO("File %s reconstructed and saved as %s", LAST_COPY_FILE, output_file);

    // Send read-complete to manager
    const char *rc = "read-complete";
    if (sendto(MANAGER_SOCK, rc, strlen(rc), 0,
               (struct sockaddr *)&MANAGER_ADDR, sizeof(MANAGER_ADDR)) < 0) {
        LOG_WARN("Failed to send read-complete to manager");
        return -1;
    }

    return 0;
}

static int failure_params(char *resp)
{
    char dss[NAME_LEN];
    int n;
    unsigned b;
    if (sscanf(resp, "FAILURE-PARAMS %255s %d %u", dss, &n, &b) != 3) {
        LOG_WARN("FAILED: FAILURE-PARAMS arguments not correct. response: %s", resp);
        return -1;
    }

    if (n <= 0) {
        LOG_WARN("Invalid disk count received in FAILURE-PARAMS");
        return -1;
    }

    if (COPY_SOCK < 0) {
        LOG_WARN("Copy socket not initialized");
        return -1;
    }

    Peer peers[16];
    if (n > (int)(sizeof(peers) / sizeof(peers[0]))) {
        LOG_WARN("n is too big");
        return -1;
    }

    // Parse disk information from FAILURE-PARAMS response
    char *cursor = resp + 15; // skip FAILURE-PARAMS
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

    // Initialize random number generator if not already done
    static int rng_initialized = 0;
    if (!rng_initialized) {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        rng_initialized = 1;
    }
    
    // Randomly select a disk to fail (0 <= i < n)
    int failed_disk = rand() % n;
    LOG_INFO("Selected disk %d (%s) for failure simulation", failed_disk, peers[failed_disk].name);

    // Send fail command to the selected disk
    char fail_cmd[256];
    snprintf(fail_cmd, sizeof(fail_cmd), "FAIL %s\n", dss);
    
    if (sendto(COPY_SOCK, fail_cmd, strlen(fail_cmd), 0,
               (struct sockaddr *)&peers[failed_disk].addr, sizeof(peers[failed_disk].addr)) < 0) {
        LOG_WARN("Failed to send fail command to disk %s", peers[failed_disk].name);
        return -1;
    }

    // receive fail-complete response
    const size_t buf_size = 1024;
    char buffer[buf_size];
    struct sockaddr_in src_addr;
    socklen_t src_len = sizeof(src_addr);
    ssize_t recv_len = recvfrom(COPY_SOCK, buffer, buf_size - 1, 0,
                                (struct sockaddr *)&src_addr, &src_len);
    
    if (recv_len < 0) {
        LOG_WARN("Failed to receive fail-complete response");
        return -1;
    }
    
    buffer[recv_len] = '\0';
    if (strncmp(buffer, "FAIL-COMPLETE", 13) != 0) {
        LOG_WARN("Unexpected response to fail command: %s", buffer);
        return -1;
    }

    LOG_INFO("Disk failure simulation completed for disk %s", peers[failed_disk].name);

    // now start recovery process according to specification
    LOG_INFO("Starting recovery process for failed disk %d (%s)", failed_disk, peers[failed_disk].name);
    
    int working_disk = (failed_disk == 0) ? 1 : 0; // Pick any disk except failed one
    
    // create a simple protocol to get file list from a working disk
    char list_cmd[256];
    snprintf(list_cmd, sizeof(list_cmd), "LIST %s\n", dss);
    
    if (sendto(COPY_SOCK, list_cmd, strlen(list_cmd), 0,
               (struct sockaddr *)&peers[working_disk].addr, sizeof(peers[working_disk].addr)) < 0) {
        LOG_WARN("Failed to send LIST command to working disk");
        return -1;
    }
    
    const size_t list_buf_size = 4096;
    char list_buffer[list_buf_size];
    struct sockaddr_in list_src_addr;
    socklen_t list_src_len = sizeof(list_src_addr);
    ssize_t list_recv_len = recvfrom(COPY_SOCK, list_buffer, list_buf_size - 1, 0,
                                     (struct sockaddr *)&list_src_addr, &list_src_len);

    if (list_recv_len < 0) {
        LOG_WARN("Failed to receive file list from working disk");
        return -1;
    }

    list_buffer[list_recv_len] = '\0';
    LOG_INFO("Received file list: %s", list_buffer);
    
    // parse file list and recover each file
    // "FILE-LIST file1_stripes file2_stripes ..."
    char *token = strtok(list_buffer, " \n");
    if (!token || strcmp(token, "FILE-LIST") != 0) {
        LOG_WARN("Invalid file list response");
        return -1;
    }
    
    // Process each file entry
    while ((token = strtok(NULL, " \n")) != NULL) {
        char file_name[NAME_LEN];
        int num_stripes;
        if (sscanf(token, "%255s_%d", file_name, &num_stripes) != 2) {
            continue; // Skip invalid entries
        }
        
        LOG_INFO("Recovering file %s with %d stripes", file_name, num_stripes);
        
        // Recover each stripe of this file
        for (int stripe = 0; stripe < num_stripes; stripe++) {
            // Read blocks from all working disks (D = {0,1,...,n} \ {failed_disk})
            unsigned char *blocks[16];
            pthread_t thread_ids[16];
            BlockReadTask *tasks[16];
            size_t block_sizes[16];
            int working_count = 0;
            
            // Initialize storage
            for (int d = 0; d < n; d++) {
                blocks[d] = NULL;
                block_sizes[d] = 0;
            }
            
            // Create read threads for all working disks
            for (int d = 0; d < n; d++) {
                if (d == failed_disk) continue; // Skip failed disk
                
                char request[256];
                int reqlen = snprintf(request, sizeof(request),
                                      "READ %s %s %d %d\n",
                                      dss, file_name, stripe, d);
                
                BlockReadTask *task = (BlockReadTask *)malloc(sizeof(BlockReadTask));
                if (!task) continue;
                
                task->dest = peers[d].addr;
                task->request = (char *)malloc(reqlen);
                if (!task->request) {
                    free(task);
                    continue;
                }
                memcpy(task->request, request, reqlen);
                task->request_len = reqlen;
                task->block_data = &blocks[d];
                task->block_size = &block_sizes[d];
                task->stripe_num = stripe;
                task->disk_idx = d;
                tasks[working_count] = task;
                
                if (pthread_create(&thread_ids[working_count], NULL, read_block_thread, task) == 0) {
                    working_count++;
                }
            }
            
            // Wait for all reads to complete
            for (int t = 0; t < working_count; t++) {
                pthread_join(thread_ids[t], NULL);
                free(tasks[t]->request);
                free(tasks[t]);
            }
            
            // XOR all working blocks to recover failed block
            unsigned char *recovered_block = (unsigned char *)malloc(b);
            if (!recovered_block) {
                for (int d = 0; d < n; d++) free(blocks[d]);
                continue;
            }
            
            memset(recovered_block, 0, b);
            for (int d = 0; d < n; d++) {
                if (d != failed_disk && blocks[d]) {
                    for (unsigned k = 0; k < b; k++) {
                        recovered_block[k] ^= blocks[d][k];
                    }
                }
            }
            
            // Determine if recovered block is DATA or PARITY
            unsigned int parity_disk = (unsigned int)n - ((stripe % (unsigned int)n) + 1);
            if (parity_disk >= (unsigned int)n) parity_disk = 0;
            
            const char *block_type = ((unsigned int)failed_disk == parity_disk) ? "PARITY" : "DATA";
            
            // Send recovered block to failed disk
            char write_header[256];
            int hdr_len = snprintf(write_header, sizeof(write_header),
                                   "WRITE %s %s %d %d %s %u\n",
                                   dss, file_name, stripe, failed_disk, block_type, b);
            
            size_t total_size = hdr_len + b;
            unsigned char *write_packet = (unsigned char *)malloc(total_size);
            if (write_packet) {
                memcpy(write_packet, write_header, hdr_len);
                memcpy(write_packet + hdr_len, recovered_block, b);
                
                sendto(COPY_SOCK, write_packet, total_size, 0,
                       (struct sockaddr *)&peers[failed_disk].addr, sizeof(peers[failed_disk].addr));
                
                free(write_packet);
            }
            
            free(recovered_block);
            for (int d = 0; d < n; d++) free(blocks[d]);
            
            LOG_INFO("Recovered stripe %d of file %s (%s block)", stripe, file_name, block_type);
        }
        
        LOG_INFO("Completed recovery of file %s", file_name);
    }
    
    LOG_INFO("Recovery process completed for failed disk %d", failed_disk);
    
    const char *rc = "recovery-complete";
    if (sendto(MANAGER_SOCK, rc, strlen(rc), 0,
               (struct sockaddr *)&MANAGER_ADDR, sizeof(MANAGER_ADDR)) < 0) {
        LOG_WARN("Failed to send recovery-complete to manager");
        return -1;
    }

    return 0;
}

static int decommission_params(char *resp)
{
    char dss[NAME_LEN];
    int n;
    unsigned b;
    if (sscanf(resp, "DECOMMISSION-PARAMS %255s %d %u", dss, &n, &b) != 3) {
        LOG_WARN("FAILED: DECOMMISSION-PARAMS arguments not correct. response: %s", resp);
        return -1;
    }
    if (n <= 0) { LOG_WARN("Invalid disk count received in DECOMMISSION-PARAMS"); return -1; }
    if (COPY_SOCK < 0) { LOG_WARN("Copy socket not initialized"); return -1; }

    Peer peers[16];
    if (n > (int)(sizeof(peers)/sizeof(peers[0]))) { LOG_WARN("n is too big"); return -1; }

    // Parse peers after header tokens (dss, n, b)
    char *cursor = resp + 20; // after "DECOMMISSION-PARAMS"
    for (int skip = 0; skip < 3 && *cursor; ) {  // skip <dss> <n> <b>
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
        for (int skip = 0; skip < 3 && *cursor; ) { // advance to next triple
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

    LOG_INFO("Starting decommission process for DSS %s with %d disks", dss, n);

    for (int i = 0; i < n; i++) {
        char fail_cmd[256];
        int len = snprintf(fail_cmd, sizeof(fail_cmd), "FAIL %s\n", dss);
        if (sendto(COPY_SOCK, fail_cmd, len, 0,
                   (struct sockaddr *)&peers[i].addr, sizeof(peers[i].addr)) < 0) {
            LOG_WARN("Failed to send FAIL to disk %s", peers[i].name);
        } else {
            LOG_INFO("Sent FAIL to %s (%s:%hu)", peers[i].name, peers[i].ip, peers[i].cport);
        }
    }

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(COPY_SOCK, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    int done[16] = {0};
    int completed = 0;
    int timeouts = 0;
    const int MAX_TIMEOUTS = 8 * n;

    while (completed < n && timeouts < MAX_TIMEOUTS) {
        char buffer[1024];
        struct sockaddr_in src; socklen_t srclen = sizeof(src);
        ssize_t r = recvfrom(COPY_SOCK, buffer, sizeof(buffer)-1, 0,
                             (struct sockaddr*)&src, &srclen);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                timeouts++; 
                continue;
            }
            LOG_WARN("recv error during decommission: %s", strerror(errno));
            break;
        }
        buffer[r] = '\0';

        int which = -1;
        for (int i = 0; i < n; i++) {
            if (src.sin_addr.s_addr == peers[i].addr.sin_addr.s_addr &&
                src.sin_port        == peers[i].addr.sin_port) {
                which = i; break;
            }
        }
        if (which < 0) {
            continue;
        }

        if (!done[which] && strncmp(buffer, "FAIL-COMPLETE", 13) == 0) {
            done[which] = 1;
            completed++;
            LOG_INFO("Received FAIL-COMPLETE from %s (%d/%d)", peers[which].name, completed, n);
        }
        // else: duplicate or unexpected, ignore
    }

    if (completed == n) {
        LOG_INFO("All %d disks completed failure simulation for DSS %s", n, dss);
    } else {
        LOG_WARN("Only %d/%d disks acknowledged decommission wipe", completed, n);
    }

    const char *dc = "decommission-complete";
    if (sendto(MANAGER_SOCK, dc, strlen(dc), 0,
               (struct sockaddr *)&MANAGER_ADDR, sizeof(MANAGER_ADDR)) < 0) {
        LOG_WARN("Failed to send decommission-complete to manager");
        return -1;
    }

    LOG_INFO("Decommission process completed for DSS %s", dss);
    return 0;
}


int receive_cmd(char *cmd_buffer, char *local_ip)
{
    printf("\nAvailable commands:\n");
    printf("  register-user\n");
    printf("  deregister-user\n");
    printf("  configure-dss <dss-name> <n> <striping-unit>\n");
    printf("  copy <file-name> <file-size> <owner>\n");
    printf("  read <dss-name> <file-name> <user-name>\n");
    printf("  disk-failure <dss-name>\n");
    printf("  decommission-dss <dss-name>\n");
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
        if (sscanf(line, "copy %255s %u %255s", file_name, &file_size, owner) == 3) {
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
    } else if (strncmp(line, "read", 4) == 0) {
        char dss_name[NAME_LEN];
        char file_name[NAME_LEN];
        char user_name[NAME_LEN];
        if (sscanf(line, "read %15s %15s %15s", dss_name, file_name, user_name) == 3) {
            snprintf(cmd_buffer, COMMAND_SIZE, "read %s %s %s", dss_name, file_name, user_name);
            strncpy(LAST_COPY_FILE, file_name, NAME_LEN - 1);
            LAST_COPY_FILE[NAME_LEN - 1] = '\0';
        } else {
            LOG_WARN("Unknown command format for read");
            free(line);
            return -1;
        }
    } else if (strncmp(line, "disk-failure", 12) == 0) {
        char dss_name[NAME_LEN];
        if (sscanf(line, "disk-failure %15s", dss_name) == 1) {
            snprintf(cmd_buffer, COMMAND_SIZE, "disk-failure %s", dss_name);
        } else {
            LOG_WARN("Unknown command format for disk-failure");
            free(line);
            return -1;
        }
    } else if (strncmp(line, "decommission-dss", 16) == 0) {
        char dss_name[NAME_LEN];
        if (sscanf(line, "decommission-dss %15s", dss_name) == 1) {
            snprintf(cmd_buffer, COMMAND_SIZE, "decommission-dss %s", dss_name);
        } else {
            LOG_WARN("Unknown command format for decommission-dss");
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

        if (strncmp(response, "READ-PARAMS ", 12) == 0) {
            if (read_params(response) == 0) {
                if ((resp_len = recvfrom(MANAGER_SOCK, response, RESPONSE_SIZE - 1, 0,
                                         (struct sockaddr*)&src_addr, &src_len)) < 0) {
                    LOG_ERROR("failed to receive post-read response");
                } else {
                    response[resp_len] = '\0';
                    if (MANAGER_ADDR.sin_addr.s_addr != src_addr.sin_addr.s_addr) {
                        LOG_ERROR("Received response from unknown source. MITM possible. Halting");
                    } else {
                        printf("[response]: %s [src ip]: %s\n", response, inet_ntoa(src_addr.sin_addr));
                    }
                }
            } else {
                printf("[response]: FAILURE: Read aborted locally [src ip]: %s\n",
                       inet_ntoa(src_addr.sin_addr));
            }
            continue;
        }

        if (strncmp(response, "FAILURE-PARAMS ", 15) == 0) {
            if (failure_params(response) == 0) {
                if ((resp_len = recvfrom(MANAGER_SOCK, response, RESPONSE_SIZE - 1, 0,
                                         (struct sockaddr*)&src_addr, &src_len)) < 0) {
                    LOG_ERROR("failed to receive post-failure response");
                } else {
                    response[resp_len] = '\0';
                    if (MANAGER_ADDR.sin_addr.s_addr != src_addr.sin_addr.s_addr) {
                        LOG_ERROR("Received response from unknown source. MITM possible. Halting");
                    } else {
                        printf("[response]: %s [src ip]: %s\n", response, inet_ntoa(src_addr.sin_addr));
                    }
                }
            } else {
                printf("[response]: FAILURE: Disk failure aborted locally [src ip]: %s\n",
                       inet_ntoa(src_addr.sin_addr));
            }
            continue;
        }

        if (strncmp(response, "DECOMMISSION-PARAMS ", 20) == 0) {
            if (decommission_params(response) == 0) {
                if ((resp_len = recvfrom(MANAGER_SOCK, response, RESPONSE_SIZE - 1, 0,
                                         (struct sockaddr*)&src_addr, &src_len)) < 0) {
                    LOG_ERROR("failed to receive post-decommission response");
                } else {
                    response[resp_len] = '\0';
                    if (MANAGER_ADDR.sin_addr.s_addr != src_addr.sin_addr.s_addr) {
                        LOG_ERROR("Received response from unknown source. MITM possible. Halting");
                    } else {
                        printf("[response]: %s [src ip]: %s\n", response, inet_ntoa(src_addr.sin_addr));
                    }
                }
            } else {
                printf("[response]: FAILURE: Decommission aborted locally [src ip]: %s\n",
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
