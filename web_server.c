#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

#define PORT 8080
#define DOC_ROOT "./www"
#define BUFFER_SIZE 1024

int server_fd; // Global server socket descriptor
int active_connections = 0; // Global active connection counter
unsigned long total_requests = 0; // Total requests served
unsigned long total_bytes_received = 0; // Total bytes received by the server
unsigned long total_bytes_transmitted = 0; // Total bytes transmitted by the server
unsigned long http_2xx = 0, http_4xx = 0, http_5xx = 0; // HTTP response code counters
time_t server_start_time; // Server start time

pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for stats

// Signal handler for cleanup
void handle_exit(int sig) {
    printf("\nShutting down the server...\n");

    if (server_fd >= 0) {
        close(server_fd);
        printf("Server socket closed.\n");
    }

    exit(0);
}

// Increment counters safely
void increment_stat(unsigned long *stat) {
    pthread_mutex_lock(&stats_mutex);
    (*stat)++;
    pthread_mutex_unlock(&stats_mutex);
}

// Update transmitted and received bytes
void update_byte_stats(unsigned long received, unsigned long transmitted) {
    pthread_mutex_lock(&stats_mutex);
    total_bytes_received += received;
    total_bytes_transmitted += transmitted;
    pthread_mutex_unlock(&stats_mutex);
}

// Serve the /stats endpoint
void serve_stats(int client_socket) {
    char stats[BUFFER_SIZE * 4];
    time_t now = time(NULL);
    unsigned long uptime = (unsigned long)(now - server_start_time);

    pthread_mutex_lock(&stats_mutex);
    snprintf(stats, sizeof(stats),
             "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n"
             "Active connections: %d\n"
             "Total requests served: %lu\n"
             "Uptime: %lu days, %02lu:%02lu:%02lu\n"
             "Total bytes received: %lu\n"
             "Total bytes transmitted: %lu\n"
             "HTTP 2xx responses: %lu\n"
             "HTTP 4xx responses: %lu\n"
             "HTTP 5xx responses: %lu\n",
             active_connections, total_requests,
             uptime / 86400, (uptime % 86400) / 3600,
             (uptime % 3600) / 60, uptime % 60,
             total_bytes_received, total_bytes_transmitted,
             http_2xx, http_4xx, http_5xx);
    pthread_mutex_unlock(&stats_mutex);

    send(client_socket, stats, strlen(stats), 0);
    close(client_socket);
}

// List all files in the document root
void list_files(int client_socket) {
    DIR *dir = opendir(DOC_ROOT);
    if (!dir) {
        increment_stat(&http_5xx);
        const char *error = "HTTP/1.1 500 Internal Server Error\r\n\r\n";
        send(client_socket, error, strlen(error), 0);
        close(client_socket);
        return;
    }

    increment_stat(&http_2xx);
    const char *header = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n";
    send(client_socket, header, strlen(header), 0);

    struct dirent *entry;
    char buffer[BUFFER_SIZE];
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) { // Only regular files
            snprintf(buffer, sizeof(buffer), "%s\n", entry->d_name);
            send(client_socket, buffer, strlen(buffer), 0);
        }
    }

    closedir(dir);
    close(client_socket);
}

// Send a specific file to the client
void send_file(int client_socket, const char *filename) {
    char full_path[BUFFER_SIZE];
    snprintf(full_path, sizeof(full_path), "%s/%s", DOC_ROOT, filename);

    FILE *file = fopen(full_path, "r");
    if (!file) {
        increment_stat(&http_4xx);
        const char *not_found =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html\r\n\r\n"
            "<h1>404 Not Found</h1>";
        send(client_socket, not_found, strlen(not_found), 0);
        close(client_socket);
        return;
    }

    increment_stat(&http_2xx);
    const char *header = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n";
    send(client_socket, header, strlen(header), 0);

    char buffer[BUFFER_SIZE];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        send(client_socket, buffer, bytes, 0);
        update_byte_stats(0, bytes);
    }

    fclose(file);
    close(client_socket);
}

// Serve a static file
void serve_file(int client_socket, const char *path) {
    char full_path[BUFFER_SIZE];
    snprintf(full_path, sizeof(full_path), "%s%s", DOC_ROOT, path);

    FILE *file = fopen(full_path, "r");
    if (!file) {
        increment_stat(&http_4xx);
        const char *not_found =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html\r\n\r\n"
            "<h1>404 Not Found</h1>";
        send(client_socket, not_found, strlen(not_found), 0);
        close(client_socket);
        return;
    }

    increment_stat(&http_2xx);
    const char *header = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n";
    send(client_socket, header, strlen(header), 0);

    char buffer[BUFFER_SIZE];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        send(client_socket, buffer, bytes, 0);
        update_byte_stats(0, bytes);
    }

    fclose(file);
    close(client_socket);
}

// Handle each client
void *handle_client(void *arg) {
    int client_socket = *(int *)arg;
    free(arg);

    pthread_mutex_lock(&stats_mutex);
    active_connections++;
    total_requests++;
    pthread_mutex_unlock(&stats_mutex);

    char buffer[BUFFER_SIZE];
    size_t bytes_received = recv(client_socket, buffer, sizeof(buffer), 0);
    update_byte_stats(bytes_received, 0);

    char method[16], path[256];
    sscanf(buffer, "%s %s", method, path);

    if (strcmp(path, "/") == 0) {
        strcpy(path, "/index.html");
    }

    if (strcmp(path, "/stats") == 0) {
        serve_stats(client_socket);
    } else if (strcmp(path, "/sync") == 0) {
        list_files(client_socket);
    } else if (strncmp(path, "/sync/", 6) == 0) {
        char filename[256];
        sscanf(path, "/sync/%s", filename); // Extract filename
        send_file(client_socket, filename); // Send requested file
    } else if (strcmp(method, "GET") == 0) {
        serve_file(client_socket, path);
    } else {
        increment_stat(&http_4xx);
        const char *not_found =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html\r\n\r\n"
            "<h1>404 Not Found</h1>";
        send(client_socket, not_found, strlen(not_found), 0);
        close(client_socket);
    }

    pthread_mutex_lock(&stats_mutex);
    active_connections--;
    pthread_mutex_unlock(&stats_mutex);

    return NULL;
}

int main() {
    struct sockaddr_in server_addr, client_addr;
    signal(SIGINT, handle_exit);

    server_start_time = time(NULL);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 5) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Server is running on port %d. Press Ctrl+C to stop.\n", PORT);

    while (1) {
        socklen_t addr_len = sizeof(client_addr);
        int *client_socket = malloc(sizeof(int));
        if ((*client_socket = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len)) < 0) {
            perror("Accept failed");
            free(client_socket);
            continue;
        }

        pthread_t thread;
        pthread_create(&thread, NULL, handle_client, client_socket);
        pthread_detach(thread);
    }

    return 0;
}

