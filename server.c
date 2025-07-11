#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sqlite3.h>
#include <signal.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <linux/limits.h>
// Removed invalid header <cstdio.h>
#include<stdlib.h>

#define PORT 8080
#define BUFFER_SIZE 4096
#define MAX_CLIENTS 10
#define MAX_USERNAME_LEN 50
#define MAX_PASSWORD_LEN 50
#define MAX_FILENAME_LEN 256
#define MAX_CODE_LEN 50

typedef struct {
    char filename[MAX_FILENAME_LEN];
    char owner[MAX_USERNAME_LEN];
    char visibility;
    char access_code[MAX_CODE_LEN];
    long filesize;
} FileInfo;

int client_sockets[MAX_CLIENTS];
char client_usernames[MAX_CLIENTS][MAX_USERNAME_LEN];
int authenticated[MAX_CLIENTS];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t files_mutex = PTHREAD_MUTEX_INITIALIZER;
int client_count = 0;
sqlite3 *db;
volatile int server_running = 1;

void log_history(const char *username, const char *action, const char *filename, long filesize) {
    char *errmsg = 0;
    char *sql = sqlite3_mprintf(
        "INSERT INTO history(username, action, filename, filesize) "
        "VALUES('%q', '%q', '%q', %ld);",
        username, action, filename, filesize);
    if (sqlite3_exec(db, sql, 0, 0, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "History error: %s\n", errmsg);
        sqlite3_free(errmsg);
    }
    sqlite3_free(sql);
}

void show_user_history(const char *username) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT action, filename, filesize, timestamp "
        "FROM history WHERE username='%s' ORDER BY timestamp DESC;",
        username);

    pthread_mutex_lock(&files_mutex);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", sqlite3_errmsg(db));
        pthread_mutex_unlock(&files_mutex);
        return;
    }

    printf("\nTransaction history for %s:\n", username);
    printf("%-19s | %-7s | %-20s | %s\n",
          "Timestamp", "Action", "Filename", "Size");
    printf("------------------------------------------------------------\n");

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *action = (const char *)sqlite3_column_text(stmt, 0);
        const char *filename = (const char *)sqlite3_column_text(stmt, 1);
        long filesize = sqlite3_column_int64(stmt, 2);
        const char *timestamp = (const char *)sqlite3_column_text(stmt, 3);
        printf("%-19s | %-7s | %-20s | %ld bytes\n",
              timestamp, action, filename, filesize);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&files_mutex);
}

void* admin_input_handler(void *arg) {
    char command[BUFFER_SIZE];
    while (server_running) {
        printf("\nServer Admin Console\n");
        printf("Available commands:\n");
        printf("/history <username> - Show user transaction history\n");
        printf("/exit - Shutdown server\n");
        printf("Enter command: ");
        fflush(stdout);

        if (fgets(command, sizeof(command), stdin) == NULL) continue;
        command[strcspn(command, "\n")] = '\0';

        if (strncmp(command, "/history ", 9) == 0) {
            char username[MAX_USERNAME_LEN];
            strncpy(username, command + 9, sizeof(username) - 1);
            username[sizeof(username) - 1] = '\0';
            show_user_history(username);
        }
        else if (strcmp(command, "/exit") == 0) {
            printf("Shutting down server...\n");
            raise(SIGINT);
        }
        else {
            printf("Invalid command\n");
        }
    }
    return NULL;
}

void handle_signal(int sig) {
    server_running = 0;
}

void init_db() {
    if (sqlite3_open("users.db", &db)) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
        exit(1);
    }

    char *errmsg = 0;
    const char *sql = 
        "CREATE TABLE IF NOT EXISTS users ("
        "username TEXT PRIMARY KEY, password TEXT);"
        "CREATE TABLE IF NOT EXISTS files ("
        "filename TEXT, owner TEXT, visibility CHAR, "
        "access_code TEXT, filesize INTEGER);"
        "CREATE TABLE IF NOT EXISTS history ("
        "username TEXT, action TEXT, filename TEXT, "
        "filesize INTEGER, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);";

    if (sqlite3_exec(db, sql, 0, 0, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", errmsg);
        sqlite3_free(errmsg);
        exit(1);
    }
}

int register_user(const char *username, const char *password) {
    char *errmsg = 0;
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT INTO users(username, password) VALUES('%s', '%s');",
        username, password);

    if (sqlite3_exec(db, sql, 0, 0, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", errmsg);
        sqlite3_free(errmsg);
        return 0;
    }
    return 1;
}

int login_user(const char *username, const char *password) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT 1 FROM users WHERE username='%s' AND password='%s';",
        username, password);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW;
}

void send_file_list(int client_socket, const char *username) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT filename, visibility FROM files "
        "WHERE owner='%s' OR visibility='P' OR "
        "(visibility='R' AND owner IN "
        "(SELECT owner FROM files WHERE owner!='%s'));",
        username, username);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", sqlite3_errmsg(db));
        send(client_socket, "ERROR: Could not retrieve file list\n", 35, 0);
        return;
    }

    send(client_socket, "[LISTSTART]\n", 12, 0);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *filename = (const char *)sqlite3_column_text(stmt, 0);
        const char *visibility = (const char *)sqlite3_column_text(stmt, 1);
        char line[512];
        snprintf(line, sizeof(line), "- %s (%s)\n", filename,
                visibility[0] == 'P' ? "Public" :
                (visibility[0] == 'R' ? "Private" : "User-only"));
        send(client_socket, line, strlen(line), 0);
    }
    send(client_socket, "[LISTEND]\n", 10, 0);
    sqlite3_finalize(stmt);
}

// RESUME: Modified to support resumable downloads with offset
void send_file(int client_socket, const char *filename, const char *owner, 
              char visibility, const char *access_code, long offset) {
    char filepath[PATH_MAX];
    snprintf(filepath, sizeof(filepath), "files/%s", filename);
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        send(client_socket, "ERROR: File not found\n", 22, 0);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long filesize = ftell(fp);
    if (offset > filesize) {
        send(client_socket, "ERROR: Invalid offset\n", 22, 0);
        fclose(fp);
        return;
    }
    fseek(fp, offset, SEEK_SET);

    if (send(client_socket, "[FILE]", 6, 0) <= 0 ||
        send(client_socket, &visibility, 1, 0) <= 0) {
        fclose(fp);
        return;
    }

    if (visibility == 'R') {
        char received_code[MAX_CODE_LEN];
        int bytes = recv(client_socket, received_code, MAX_CODE_LEN - 1, 0);
        if (bytes <= 0) {
            fclose(fp);
            return;
        }
        received_code[bytes] = '\0';
        received_code[strcspn(received_code, "\n")] = '\0';

        if (strcmp(received_code, access_code) != 0) {
            send(client_socket, "ERROR: Incorrect access code\n", 29, 0);
            fclose(fp);
            return;
        }
    }

    int filename_len = strlen(filename);
    int net_filename_len = htonl(filename_len);
    if (send(client_socket, &net_filename_len, sizeof(net_filename_len), 0) <= 0 ||
        send(client_socket, filename, filename_len, 0) <= 0) {
        fclose(fp);
        return;
    }

    long net_filesize = htonl(filesize);
    if (send(client_socket, &net_filesize, sizeof(net_filesize), 0) <= 0) {
        fclose(fp);
        return;
    }

    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, fp)) > 0) {
        if (send(client_socket, buffer, bytes_read, 0) <= 0) break;
    }
    fclose(fp);
}

int add_file_to_db(const char *filename, const char *owner, char visibility,
                  const char *access_code, long filesize) {
    char *errmsg = 0;
    char *sql = sqlite3_mprintf(
        "INSERT INTO files(filename, owner, visibility, access_code, filesize) "
        "VALUES('%q', '%q', '%c', '%q', %ld);",
        filename, owner, visibility, access_code, filesize);

    if (sqlite3_exec(db, sql, 0, 0, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", errmsg);
        sqlite3_free(errmsg);
        return 0;
    }
    return 1;
}

void *client_handler(void *arg) {
    int client_socket = *(int *)arg;
    free(arg);
    char buffer[BUFFER_SIZE];
    char username[MAX_USERNAME_LEN] = {0};
    int authed = 0;

    while (!authed && server_running) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) break;
        buffer[bytes] = '\0';

        if (strncmp(buffer, "/register ", 10) == 0) {
            char *space = strchr(buffer + 10, ' ');
            if (!space) {
                send(client_socket, "Usage: /register username password\n", 34, 0);
                continue;
            }
            char password[MAX_PASSWORD_LEN];
            strncpy(username, buffer + 10, space - (buffer + 10));
            username[space - (buffer + 10)] = '\0';
            strncpy(password, space + 1, sizeof(password) - 1);
            password[sizeof(password) - 1] = '\0';

            if (register_user(username, password)) {
                send(client_socket, "Registered successfully.\n", 26, 0);
                authed = 1;
                pthread_mutex_lock(&clients_mutex);
                for (int i = 0; i < client_count; i++) {
                    if (client_sockets[i] == client_socket) {
                        authenticated[i] = 1;
                        strcpy(client_usernames[i], username);
                        break;
                    }
                }
                pthread_mutex_unlock(&clients_mutex);
            } else {
                send(client_socket, "Registration failed. Username may already exist.\n", 48, 0);
            }
        }
        else if (strncmp(buffer, "/login ", 7) == 0) {
            char *space = strchr(buffer + 7, ' ');
            if (!space) {
                send(client_socket, "Usage: /login username password\n", 32, 0);
                continue;
            }
            char password[MAX_PASSWORD_LEN];
            strncpy(username, buffer + 7, space - (buffer + 7));
            username[space - (buffer + 7)] = '\0';
            strncpy(password, space + 1, sizeof(password) - 1);
            password[sizeof(password) - 1] = '\0';

            if (login_user(username, password)) {
                send(client_socket, "Login successful.\n", 18, 0);
                authed = 1;
                pthread_mutex_lock(&clients_mutex);
                for (int i = 0; i < client_count; i++) {
                    if (client_sockets[i] == client_socket) {
                        authenticated[i] = 1;
                        strcpy(client_usernames[i], username);
                        break;
                    }
                }
                pthread_mutex_unlock(&clients_mutex);
            } else {
                send(client_socket, "Invalid credentials.\n", 21, 0);
            }
        } else {
            send(client_socket, "Please /login or /register first.\n", 34, 0);
        }
    }

    while (authed && server_running) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) break;
        buffer[bytes] = '\0';

        // RESUME: Parse offset from download command
        if (strncmp(buffer, "/download ", 10) == 0) {
            char *token = strtok(buffer + 10, " ");
            char *filename = token;
            char *offset_str = strtok(NULL, " ");
            long offset = (offset_str) ? atol(offset_str) : 0;

            pthread_mutex_lock(&files_mutex);
            const char *sql = "SELECT filename, owner, visibility, access_code, filesize FROM files WHERE filename = ? LIMIT 1;";
            sqlite3_stmt *stmt;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, filename, -1, SQLITE_STATIC);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char *db_filename = (const char *)sqlite3_column_text(stmt, 0);
                    const char *owner = (const char *)sqlite3_column_text(stmt, 1);
                    const char *visibility_str = (const char *)sqlite3_column_text(stmt, 2);
                    const char *access_code = (const char *)sqlite3_column_text(stmt, 3);
                    long filesize = sqlite3_column_int64(stmt, 4);
                    char visibility = visibility_str ? visibility_str[0] : 'P';

                    log_history(username, "DOWNLOAD", db_filename, filesize);
                    send_file(client_socket, db_filename, owner, visibility, access_code, offset);
                } else {
                    send(client_socket, "ERROR: File not found\n", 22, 0);
                }
                sqlite3_finalize(stmt);
            }
            pthread_mutex_unlock(&files_mutex);
        }
        else if (strncmp(buffer, "[FILE]", 6) == 0) {
            char visibility;
            if (recv(client_socket, &visibility, 1, 0) <= 0) {
                perror("Visibility receive failed");
                continue;
            }

            char access_code[MAX_CODE_LEN] = {0};
            if (visibility == 'R') {
                send(client_socket, "Enter access code: ", 19, 0);
                bytes = recv(client_socket, access_code, MAX_CODE_LEN - 1, 0);
                if (bytes <= 0) {
                    perror("Access code receive failed");
                    continue;
                }
                access_code[bytes] = '\0';
            }

            int net_filename_len;
            if (recv(client_socket, &net_filename_len, sizeof(net_filename_len), 0) <= 0) {
                perror("Filename length receive failed");
                continue;
            }
            int filename_len = ntohl(net_filename_len);
            char filename[MAX_FILENAME_LEN];
            if (recv(client_socket, filename, filename_len, 0) <= 0) {
                perror("Filename receive failed");
                continue;
            }
            filename[filename_len] = '\0';

            long net_filesize;
            if (recv(client_socket, &net_filesize, sizeof(net_filesize), 0) <= 0) {
                perror("File size receive failed");
                continue;
            }
            long filesize = ntohl(net_filesize);

            char filepath[PATH_MAX];
            snprintf(filepath, sizeof(filepath), "files/%s", filename);
            FILE *fp = fopen(filepath, "wb");
            if (!fp) {
                perror("Failed to create file");
                send(client_socket, "ERROR: Failed to save file\n", 27, 0);
                continue;
            }

            long remaining = filesize;
            while (remaining > 0 && server_running) {
                int to_read = remaining > BUFFER_SIZE ? BUFFER_SIZE : remaining;
                bytes = recv(client_socket, buffer, to_read, 0);
                if (bytes <= 0) break;
                if (fwrite(buffer, 1, bytes, fp) != bytes) {
                    perror("File write failed");
                    break;
                }
                remaining -= bytes;
            }

            fclose(fp);
            if (remaining == 0) {
                pthread_mutex_lock(&files_mutex);
                if (add_file_to_db(filename, username, visibility, access_code, filesize)) {
                    log_history(username, "UPLOAD", filename, filesize);
                    send(client_socket, "File uploaded successfully\n", 27, 0);
                } else {
                    send(client_socket, "ERROR: Failed to register file\n", 30, 0);
                }
                pthread_mutex_unlock(&files_mutex);
            } else {
                remove(filepath);
                send(client_socket, "ERROR: Upload incomplete\n", 25, 0);
            }
        }
        else if (strcmp(buffer, "/list") == 0) {
            send_file_list(client_socket, username);
        }
        else {
            printf("Client %s (%d): %s\n", username, client_socket, buffer);
        }
    }

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        if (client_sockets[i] == client_socket) {
            for (int j = i; j < client_count - 1; j++) {
                client_sockets[j] = client_sockets[j + 1];
                authenticated[j] = authenticated[j + 1];
                strcpy(client_usernames[j], client_usernames[j + 1]);
            }
            client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    close(client_socket);
    return NULL;
}

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    pthread_t admin_thread;

    if (pthread_create(&admin_thread, NULL, admin_input_handler, NULL) != 0) {
        perror("Failed to create admin thread");
        exit(EXIT_FAILURE);
    }

    if (mkdir("files", 0755) == -1 && errno != EEXIST) {
        perror("Failed to create files directory");
        exit(EXIT_FAILURE);
    }

    init_db();
    int server_fd;
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("Setsockopt failed");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Server started on port %d\n", PORT);
    printf("Waiting for clients to connect...\n");

    while (server_running) {
        int *new_sock = malloc(sizeof(int));
        *new_sock = accept(server_fd, (struct sockaddr *)&address, &addrlen);
        if (*new_sock < 0) {
            if (server_running) perror("Accept failed");
            free(new_sock);
            continue;
        }

        pthread_mutex_lock(&clients_mutex);
        if (client_count < MAX_CLIENTS) {
            client_sockets[client_count] = *new_sock;
            authenticated[client_count] = 0;
            client_usernames[client_count][0] = '\0';
            client_count++;
            printf("New client connected (%d). Total clients: %d\n", *new_sock, client_count);
            
            pthread_t client_thread;
            if (pthread_create(&client_thread, NULL, client_handler, new_sock) != 0) {
                perror("Failed to create thread");
                close(*new_sock);
                client_count--;
            } else {
                pthread_detach(client_thread);
            }
        } else {
            printf("Max clients reached. Connection rejected.\n");
            close(*new_sock);
            free(new_sock);
        }
        pthread_mutex_unlock(&clients_mutex);
    }

    printf("Shutting down server...\n");
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        close(client_sockets[i]);
    }
    pthread_mutex_unlock(&clients_mutex);
    sqlite3_close(db);
    close(server_fd);
    pthread_join(admin_thread, NULL);
    pthread_mutex_destroy(&clients_mutex);
    pthread_mutex_destroy(&files_mutex);
    return 0;
}