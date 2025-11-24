#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <sys/time.h>
#include <pthread.h>
#include <mqueue.h>
#include <fcntl.h>
#include <sys/stat.h>

#define GRID_SIZE 10
#define MAX_STRING_LEN 64
#define MSG_SIZE 512

// Message types
#define MSG_READ 1
#define MSG_WRITE 2
#define MSG_READ_RESPONSE 3
#define MSG_WRITE_RESPONSE 4
#define MSG_SHUTDOWN 5
#define MSG_PRINT_DOC_READ 6
#define MSG_UNLOCK 7

// Response codes
#define RESP_SUCCESS 0
#define RESP_DENIED 1

// Timestamped printf that prefixes each line with HH:MM:SS.mmm
static void ts_printf(const char *fmt, ...) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_info;
    localtime_r(&tv.tv_sec, &tm_info);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_info);

    fprintf(stdout, "%s.%03ld ", tbuf, tv.tv_usec / 1000);

    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);

    fflush(stdout);
}

#define printf(...) ts_printf(__VA_ARGS__)

void msleep(int milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

// Message structure
typedef struct {
    long mtype;
    int client_id;
    int line;
    int word_pos;
    char data[MAX_STRING_LEN];
    int duration_ms;
    int response_code;
} Message;

int client_id_global;
mqd_t server_mq;
mqd_t client_mq;
volatile int print_thread_running = 1;
pthread_mutex_t doc_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t comm_mutex = PTHREAD_MUTEX_INITIALIZER;
char document[GRID_SIZE][GRID_SIZE][MAX_STRING_LEN];

// Pass the document string to be printed
void print_doc_to_file(const char* doc) {
    char filename[64];
    snprintf(filename, sizeof(filename), "output_client%d.txt", client_id_global);

    FILE* fp = fopen(filename, "w");
    if (fp) {
        fprintf(fp, "%s", doc);
        fclose(fp);
    }
}

void update_and_print_document() {
    // Fetch current document state from server for positions we don't have locally
    // This updates positions that might be locked (will show ???) or have been
    // updated by other clients
    for (int i = 0; i < GRID_SIZE; i++) {
        for (int j = 0; j < GRID_SIZE; j++) {
            Message msg;
            msg.mtype = MSG_PRINT_DOC_READ;
            msg.client_id = client_id_global;
            msg.line = i;
            msg.word_pos = j;
            
            pthread_mutex_lock(&comm_mutex);
            if (mq_send(server_mq, (char*)&msg, sizeof(Message), 0) == -1) {
                pthread_mutex_unlock(&comm_mutex);
                continue;
            }
            
            Message response;
            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_nsec += 200000000; // 200ms timeout
            if (timeout.tv_nsec >= 1000000000) {
                timeout.tv_sec += 1;
                timeout.tv_nsec -= 1000000000;
            }
            
            ssize_t bytes = mq_timedreceive(client_mq, (char*)&response, sizeof(Message), NULL, &timeout);
            pthread_mutex_unlock(&comm_mutex);
            
            pthread_mutex_lock(&doc_mutex);
            if (bytes > 0) {
                if (response.response_code == RESP_SUCCESS) {
                    strncpy(document[i][j], response.data, MAX_STRING_LEN);
                    document[i][j][MAX_STRING_LEN - 1] = '\0';
                } else {
                    // Word is locked - mark as ???
                    strcpy(document[i][j], "???");
                }
            }
            // If timeout, keep existing value in local cache
            pthread_mutex_unlock(&doc_mutex);
        }
    }
    
    // Build document string from local cache
    char doc_str[GRID_SIZE * GRID_SIZE * (MAX_STRING_LEN + 1)] = "";
    
    pthread_mutex_lock(&doc_mutex);
    for (int i = 0; i < GRID_SIZE; i++) {
        int has_data = 0;
        char line_buffer[GRID_SIZE * (MAX_STRING_LEN + 1)] = "";
        
        for (int j = 0; j < GRID_SIZE; j++) {
            if (strlen(document[i][j]) > 0) {
                if (has_data) {
                    strcat(line_buffer, " ");
                }
                strcat(line_buffer, document[i][j]);
                has_data = 1;
            }
        }
        
        if (has_data) {
            if (strlen(doc_str) > 0) {
                strcat(doc_str, "\n");
            }
            strcat(doc_str, line_buffer);
        }
    }
    pthread_mutex_unlock(&doc_mutex);
    
    print_doc_to_file(doc_str);
}

void* print_doc_thread(void* arg) {
    (void)arg;
    
    while (print_thread_running) {
        for (int i = 0; i < 40 && print_thread_running; i++) {
            msleep(50);
        }
        
        if (!print_thread_running) break;
        
        update_and_print_document();
    }
    
    return NULL;
}

void send_write_request(int line, int word_pos, const char* data, int duration) {
    printf("Client %d: Requesting WRITE lock for (%d,%d)\n", client_id_global, line, word_pos);
    
    Message msg;
    msg.mtype = MSG_WRITE;
    msg.client_id = client_id_global;
    msg.line = line;
    msg.word_pos = word_pos;
    strncpy(msg.data, data, MAX_STRING_LEN);
    msg.data[MAX_STRING_LEN - 1] = '\0';
    msg.duration_ms = duration;
    
    pthread_mutex_lock(&comm_mutex);
    mq_send(server_mq, (char*)&msg, sizeof(Message), 0);
    
    Message response;
    mq_receive(client_mq, (char*)&response, sizeof(Message), NULL);
    pthread_mutex_unlock(&comm_mutex);
    
    if (response.response_code == RESP_SUCCESS) {
        printf("Client %d: WRITE(%d,%d) = '%s', sleeping for %dms\n", 
               client_id_global, line, word_pos, data, duration);
        
        pthread_mutex_lock(&doc_mutex);
        strncpy(document[line][word_pos], data, MAX_STRING_LEN);
        document[line][word_pos][MAX_STRING_LEN - 1] = '\0';
        pthread_mutex_unlock(&doc_mutex);
        
        msleep(duration);
        
        printf("Client %d: WRITE(%d,%d) COMPLETED\n", client_id_global, line, word_pos);
        
        // Send unlock message
        Message unlock_msg;
        unlock_msg.mtype = MSG_UNLOCK;
        unlock_msg.client_id = client_id_global;
        unlock_msg.line = line;
        unlock_msg.word_pos = word_pos;
        
        pthread_mutex_lock(&comm_mutex);
        mq_send(server_mq, (char*)&unlock_msg, sizeof(Message), 0);
        pthread_mutex_unlock(&comm_mutex);
    } else {
        printf("Client %d: WRITE(%d,%d) DROPPED\n", client_id_global, line, word_pos);
    }
}

void send_read_request(int line, int word_pos) {
    printf("Client %d: Requesting READ lock for (%d,%d)\n", client_id_global, line, word_pos);
    
    Message msg;
    msg.mtype = MSG_READ;
    msg.client_id = client_id_global;
    msg.line = line;
    msg.word_pos = word_pos;
    
    pthread_mutex_lock(&comm_mutex);
    mq_send(server_mq, (char*)&msg, sizeof(Message), 0);
    
    Message response;
    mq_receive(client_mq, (char*)&response, sizeof(Message), NULL);
    pthread_mutex_unlock(&comm_mutex);
    
    if (response.response_code == RESP_SUCCESS) {
        printf("Client %d: READ(%d,%d) SUCCESS - Value: '%s'\n", 
               client_id_global, line, word_pos, response.data);
        
        pthread_mutex_lock(&doc_mutex);
        strncpy(document[line][word_pos], response.data, MAX_STRING_LEN);
        document[line][word_pos][MAX_STRING_LEN - 1] = '\0';
        pthread_mutex_unlock(&doc_mutex);
    } else {
        printf("Client %d: READ(%d,%d) DROPPED\n", client_id_global, line, word_pos);
    }
}

int main(int argc, char* argv[]) {
    // Set line buffering for stdout
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <client_id>\n", argv[0]);
        return 1;
    }

    int client_id = atoi(argv[1]);
    client_id_global = client_id;
    printf("Client %d: Starting\n", client_id);
    
    // Initialize document
    for (int i = 0; i < GRID_SIZE; i++) {
        for (int j = 0; j < GRID_SIZE; j++) {
            memset(document[i][j], 0, MAX_STRING_LEN);
        }
    }
    
    // Open server message queue
    server_mq = mq_open("/server_queue", O_WRONLY);
    if (server_mq == (mqd_t)-1) {
        perror("Client: Failed to open server queue");
        return 1;
    }
    
    // Create client message queue
    char qname[64];
    snprintf(qname, sizeof(qname), "/client_queue_%d", client_id);
    
    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(Message);
    attr.mq_curmsgs = 0;
    
    mq_unlink(qname);
    client_mq = mq_open(qname, O_CREAT | O_RDONLY, 0644, &attr);
    if (client_mq == (mqd_t)-1) {
        perror("Client: Failed to create client queue");
        return 1;
    }
    
    // Start print_doc thread
    pthread_t print_thread;
    pthread_create(&print_thread, NULL, print_doc_thread, NULL);
    
    // Read and process input.txt
    FILE* fp = fopen("input.txt", "r");
    if (!fp) {
        perror("Client: Failed to open input.txt");
        return 1;
    }
    
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        int cid;
        char cmd[16];
        
        if (sscanf(line, "C%d %s", &cid, cmd) != 2) continue;
        if (cid != client_id) continue;
        
        if (strcmp(cmd, "WRITE") == 0) {
            int ln, wp, duration;
            char data[MAX_STRING_LEN];
            if (sscanf(line, "C%d WRITE %d %d %s %d", &cid, &ln, &wp, data, &duration) == 5) {
                send_write_request(ln, wp, data, duration);
            }
        } else if (strcmp(cmd, "READ") == 0) {
            int ln, wp;
            if (sscanf(line, "C%d READ %d %d", &cid, &ln, &wp) == 3) {
                send_read_request(ln, wp);
            }
        } else if (strcmp(cmd, "SLEEP") == 0) {
            int duration;
            if (sscanf(line, "C%d SLEEP %d", &cid, &duration) == 2) {
                printf("Client %d: Sleeping for %dms\n", client_id, duration);
                msleep(duration);
            }
        }
    }
    
    fclose(fp);
    
    printf("Client %d: Finished processing commands\n", client_id);
    
    // Stop print thread and wait for it to finish
    print_thread_running = 0;
    pthread_join(print_thread, NULL);
    
    // DON'T update the output file here
    // Keep whatever the periodic print_doc thread last wrote
    // This preserves ??? for words that were locked during snapshots
    
    // Cleanup
    mq_close(server_mq);
    mq_close(client_mq);
    mq_unlink(qname);
    
    printf("Client %d: Finished\n", client_id);
    return 0;
}
