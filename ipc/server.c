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
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>

#define GRID_SIZE 10
#define MAX_STRING_LEN 64
#define MAX_CLIENTS 10
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

// Word cell structure
typedef struct {
    char data[MAX_STRING_LEN];
    pthread_mutex_t lock;
    int locked_by;  // client id, -1 if not locked
} WordCell;

// Document structure
typedef struct {
    WordCell cells[GRID_SIZE][GRID_SIZE];
} Document;

Document doc;
mqd_t server_mq;
mqd_t client_mqs[MAX_CLIENTS];
volatile int running = 1;

void msleep(int milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

void handle_unlock_request(Message* msg) {
    pthread_mutex_lock(&doc.cells[msg->line][msg->word_pos].lock);
    
    if (doc.cells[msg->line][msg->word_pos].locked_by == msg->client_id) {
        doc.cells[msg->line][msg->word_pos].locked_by = -1;
        printf("Server: Client %d UNLOCK(%d,%d)\n", msg->client_id, msg->line, msg->word_pos);
    }
    
    pthread_mutex_unlock(&doc.cells[msg->line][msg->word_pos].lock);
}

void handle_write_request(Message* msg) {
    // Open client queue if not already open
    if (client_mqs[msg->client_id] == (mqd_t)-1) {
        char qname[64];
        snprintf(qname, sizeof(qname), "/client_queue_%d", msg->client_id);
        client_mqs[msg->client_id] = mq_open(qname, O_WRONLY);
        if (client_mqs[msg->client_id] == (mqd_t)-1) {
            printf("Server: Failed to open client %d queue\n", msg->client_id);
            return;
        }
    }
    
    pthread_mutex_lock(&doc.cells[msg->line][msg->word_pos].lock);
    
    if (doc.cells[msg->line][msg->word_pos].locked_by != -1) {
        // Word is locked, deny request
        pthread_mutex_unlock(&doc.cells[msg->line][msg->word_pos].lock);
        
        printf("Server: Client %d WRITE LOCK(%d,%d) DENIED\n", 
               msg->client_id, msg->line, msg->word_pos);
        
        Message response;
        response.mtype = MSG_WRITE_RESPONSE;
        response.client_id = msg->client_id;
        response.response_code = RESP_DENIED;
        response.line = msg->line;
        response.word_pos = msg->word_pos;
        
        mq_send(client_mqs[msg->client_id], (char*)&response, sizeof(Message), 0);
        return;
    }
    
    // Grant lock
    doc.cells[msg->line][msg->word_pos].locked_by = msg->client_id;
    strncpy(doc.cells[msg->line][msg->word_pos].data, msg->data, MAX_STRING_LEN);
    doc.cells[msg->line][msg->word_pos].data[MAX_STRING_LEN - 1] = '\0';
    
    printf("Server: Client %d WRITE LOCK(%d,%d) GRANTED\n", 
           msg->client_id, msg->line, msg->word_pos);
    
    pthread_mutex_unlock(&doc.cells[msg->line][msg->word_pos].lock);
    
    // Send success response
    Message response;
    response.mtype = MSG_WRITE_RESPONSE;
    response.client_id = msg->client_id;
    response.response_code = RESP_SUCCESS;
    response.line = msg->line;
    response.word_pos = msg->word_pos;
    
    mq_send(client_mqs[msg->client_id], (char*)&response, sizeof(Message), 0);
}

void handle_read_request(Message* msg, int is_print_doc) {
    // Open client queue if not already open
    if (client_mqs[msg->client_id] == (mqd_t)-1) {
        char qname[64];
        snprintf(qname, sizeof(qname), "/client_queue_%d", msg->client_id);
        client_mqs[msg->client_id] = mq_open(qname, O_WRONLY);
        if (client_mqs[msg->client_id] == (mqd_t)-1) {
            printf("Server: Failed to open client %d queue\n", msg->client_id);
            return;
        }
    }
    
    pthread_mutex_lock(&doc.cells[msg->line][msg->word_pos].lock);
    
    if (doc.cells[msg->line][msg->word_pos].locked_by != -1) {
        // Word is locked, deny request
        pthread_mutex_unlock(&doc.cells[msg->line][msg->word_pos].lock);
        
        if (is_print_doc) {
            printf("Server: Client %d PRINT_DOC READ(%d,%d) DROPPED\n", 
                   msg->client_id, msg->line, msg->word_pos);
        } else {
            printf("Server: Client %d READ LOCK(%d,%d) DENIED\n", 
                   msg->client_id, msg->line, msg->word_pos);
        }
        
        Message response;
        response.mtype = MSG_READ_RESPONSE;
        response.client_id = msg->client_id;
        response.response_code = RESP_DENIED;
        response.line = msg->line;
        response.word_pos = msg->word_pos;
        strcpy(response.data, "???");
        
        mq_send(client_mqs[msg->client_id], (char*)&response, sizeof(Message), 0);
        return;
    }
    
    // Grant read access
    if (!is_print_doc) {
        printf("Server: Client %d READ LOCK(%d,%d) GRANTED\n", 
               msg->client_id, msg->line, msg->word_pos);
    }
    
    Message response;
    response.mtype = MSG_READ_RESPONSE;
    response.client_id = msg->client_id;
    response.response_code = RESP_SUCCESS;
    response.line = msg->line;
    response.word_pos = msg->word_pos;
    strncpy(response.data, doc.cells[msg->line][msg->word_pos].data, MAX_STRING_LEN);
    response.data[MAX_STRING_LEN - 1] = '\0';
    
    pthread_mutex_unlock(&doc.cells[msg->line][msg->word_pos].lock);
    
    mq_send(client_mqs[msg->client_id], (char*)&response, sizeof(Message), 0);
}

void print_doc_to_file() {
    FILE* fp = fopen("output.txt", "w");
    if (!fp) return;
    
    for (int i = 0; i < GRID_SIZE; i++) {
        int has_data = 0;
        char line_buffer[GRID_SIZE * (MAX_STRING_LEN + 1)] = "";
        
        for (int j = 0; j < GRID_SIZE; j++) {
            pthread_mutex_lock(&doc.cells[i][j].lock);
            if (strlen(doc.cells[i][j].data) > 0) {
                if (has_data) {
                    strcat(line_buffer, " ");
                }
                strcat(line_buffer, doc.cells[i][j].data);
                has_data = 1;
            }
            pthread_mutex_unlock(&doc.cells[i][j].lock);
        }
        
        if (has_data) {
            fprintf(fp, "%s\n", line_buffer);
        }
    }
    
    fclose(fp);
}

void signal_handler(int sig) {
    (void)sig;
    printf("Server: Received shutdown signal\n");
    running = 0;
}

int main() {
    // Set line buffering for stdout
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    
    printf("Server: Started. Grid initialized.\n");
    
    // Initialize document
    for (int i = 0; i < GRID_SIZE; i++) {
        for (int j = 0; j < GRID_SIZE; j++) {
            memset(doc.cells[i][j].data, 0, MAX_STRING_LEN);
            pthread_mutex_init(&doc.cells[i][j].lock, NULL);
            doc.cells[i][j].locked_by = -1;
        }
    }
    
    // Setup signal handler
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    
    // Create server message queue
    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(Message);
    attr.mq_curmsgs = 0;
    
    mq_unlink("/server_queue");
    server_mq = mq_open("/server_queue", O_CREAT | O_RDONLY, 0644, &attr);
    if (server_mq == (mqd_t)-1) {
        perror("Server: Failed to create message queue");
        return 1;
    }
    
    // Initialize client message queue handles
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_mqs[i] = (mqd_t)-1;
    }
    
    // Main message loop
    Message msg;
    time_t last_message_time = time(NULL);
    int idle_timeout_seconds = 1; // Exit if no messages for 1 second
    
    while (running) {
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_nsec += 50000000; // 50ms timeout
        if (timeout.tv_nsec >= 1000000000) {
            timeout.tv_sec += 1;
            timeout.tv_nsec -= 1000000000;
        }
        
        ssize_t bytes = mq_timedreceive(server_mq, (char*)&msg, sizeof(Message), NULL, &timeout);
        
        if (bytes > 0) {
            last_message_time = time(NULL);
            
            if (msg.mtype == MSG_WRITE) {
                handle_write_request(&msg);
            } else if (msg.mtype == MSG_READ) {
                handle_read_request(&msg, 0);
            } else if (msg.mtype == MSG_PRINT_DOC_READ) {
                handle_read_request(&msg, 1);
            } else if (msg.mtype == MSG_UNLOCK) {
                handle_unlock_request(&msg);
            } else if (msg.mtype == MSG_SHUTDOWN) {
                running = 0;
            }
        }
        
        // Exit if idle for too long (no client messages)
        if (time(NULL) - last_message_time > idle_timeout_seconds) {
            break;
        }
    }
    
    // Write final document
    print_doc_to_file();
    
    // Cleanup
    mq_close(server_mq);
    mq_unlink("/server_queue");
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_mqs[i] != (mqd_t)-1) {
            mq_close(client_mqs[i]);
        }
    }
    
    for (int i = 0; i < GRID_SIZE; i++) {
        for (int j = 0; j < GRID_SIZE; j++) {
            pthread_mutex_destroy(&doc.cells[i][j].lock);
        }
    }
    
    printf("Server: Shutdown complete\n");
    return 0;
}
