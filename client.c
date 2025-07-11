#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <linux/limits.h>

#define PORT 8080
#define BUFFER_SIZE 4096
#define MAX_USERNAME_LEN 50
#define MAX_PASSWORD_LEN 50
#define MAX_FILENAME_LEN 256
#define MAX_CODE_LEN 50
#define PROGRESS_BAR_WIDTH 50
#define DOWNLOAD_FOLDER "client_downloads"
#define PARTIAL_EXT ".part"

int sock;
volatile int server_running = 1;

void format_time(int seconds, char* buffer, size_t size) {
    if (seconds < 60) {
        snprintf(buffer, size, "%ds", seconds);
    } else if (seconds < 3600) {
        snprintf(buffer, size, "%dm %ds", seconds/60, seconds%60);
    } else {
        snprintf(buffer, size, "%dh %dm", seconds/3600, (seconds%3600)/60);
    }
}

void ensure_download_folder() {
    struct stat st = {0};
    if (stat(DOWNLOAD_FOLDER, &st) == -1) {
        if (mkdir(DOWNLOAD_FOLDER, 0755) == -1) {
            perror("Failed to create download folder");
            exit(EXIT_FAILURE);
        }
    }
}

void show_progress(long current, long total, double speed) {
    double percentage = (double)current / total;
    int pos = (int)round(percentage * PROGRESS_BAR_WIDTH);
    
    int eta_seconds = speed > 0 ? (int)round((total - current) / speed) : 0;
    char eta_str[20];
    format_time(eta_seconds, eta_str, sizeof(eta_str));
    
    printf("\r[");
    for (int i = 0; i < PROGRESS_BAR_WIDTH; ++i) {
        if (i < pos) printf("=");
        else if (i == pos) printf(">");
        else printf(" ");
    }
    printf("] %.1f%% (%.2f MB/s) ETA: %s", 
          percentage * 100, 
          speed/(1024*1024), 
          eta_str);
    fflush(stdout);
}

void list_files() {
    DIR *d;
    struct dirent *dir;
    d = opendir(".");
    if (d) {
        printf("\nAvailable files in current directory:\n");
        while ((dir = readdir(d)) != NULL) {
            struct stat file_stat;
            if (stat(dir->d_name, &file_stat) == 0 && S_ISREG(file_stat.st_mode)) {
                printf("- %s\n", dir->d_name);
            }
        }
        closedir(d);
    }
    printf("You: ");
    fflush(stdout);
}

void handle_file_download(char visibility, char* buffer, int initial_bytes) {
    char *ptr = buffer;
    int remaining_bytes = initial_bytes;
    
    if (visibility == 'R') {
        fflush(stdout);
        printf("\nThis file requires an access code: ");
        fflush(stdout);
        
        char access_code[MAX_CODE_LEN];
        if (!fgets(access_code, sizeof(access_code), stdin)) {
            perror("Input error");
            return;
        }
        access_code[strcspn(access_code, "\n")] = '\0';
        send(sock, access_code, strlen(access_code), 0);
        
        char response[BUFFER_SIZE];
        int bytes = recv(sock, response, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) {
            perror("Access verification failed");
            return;
        }
        response[bytes] = '\0';
        
        if (strstr(response, "ERROR")) {
            printf("%s\n", response);
            printf("You: ");
            fflush(stdout);
            return;
        }
        ptr = response;
        remaining_bytes = bytes;
    }

    int net_filename_len;
    memcpy(&net_filename_len, ptr, sizeof(net_filename_len));
    ptr += sizeof(net_filename_len);
    remaining_bytes -= sizeof(net_filename_len);
    
    int filename_len = ntohl(net_filename_len);
    char filename[MAX_FILENAME_LEN];
    
    if (filename_len > remaining_bytes) {
        strncpy(filename, ptr, remaining_bytes);
        int needed = filename_len - remaining_bytes;
        if (recv(sock, filename + remaining_bytes, needed, 0) <= 0) {
            perror("Filename receive failed");
            return;
        }
    } else {
        strncpy(filename, ptr, filename_len);
    }
    filename[filename_len] = '\0';

    long net_filesize;
    if (recv(sock, &net_filesize, sizeof(net_filesize), 0) <= 0) {
        perror("File size receive failed");
        return;
    }
    long filesize = ntohl(net_filesize);

    char part_file[PATH_MAX];
    snprintf(part_file, sizeof(part_file), "%s/%s%s", 
            DOWNLOAD_FOLDER, filename, PARTIAL_EXT);
    
    FILE *fp = fopen(part_file, "ab");
    if (!fp) {
        perror("Failed to open .part file");
        return;
    }

    long total_received = ftell(fp);
    long remaining = filesize - total_received;

    printf("\nDownloading %s (%ld bytes) - Resuming from %ld bytes\n", 
          filename, filesize, total_received);
    
    time_t start_time = time(NULL);
    time_t last_update = start_time;
    long last_bytes = total_received;
    double speed = 0;

    while (remaining > 0 && server_running) {
        int chunk = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining;
        int bytes = recv(sock, buffer, chunk, 0);
        if (bytes <= 0) {
            perror("File content receive failed");
            fclose(fp);
            remove(part_file);
            break;
        }
        
        if (fwrite(buffer, 1, bytes, fp) != bytes) {
            perror("File write failed");
            fclose(fp);
            remove(part_file);
            break;
        }
        
        remaining -= bytes;
        total_received += bytes;
        
        time_t now = time(NULL);
        if (now - last_update >= 1) {
            speed = (total_received - last_bytes) / (double)(now - last_update);
            show_progress(total_received, filesize, speed);
            last_update = now;
            last_bytes = total_received;
        }
    }

    if (remaining == 0) {
        char final_file[PATH_MAX];
        snprintf(final_file, sizeof(final_file), "%s/%s", DOWNLOAD_FOLDER, filename);
        
        int counter = 1;
        while (access(final_file, F_OK) == 0) {
            char *dot = strrchr(filename, '.');
            if (dot) {
                char base[MAX_FILENAME_LEN];
                strncpy(base, filename, dot - filename);
                base[dot - filename] = '\0';
                snprintf(final_file, sizeof(final_file), "%s/%s_%d%s", 
                       DOWNLOAD_FOLDER, base, counter++, dot);
            } else {
                snprintf(final_file, sizeof(final_file), "%s/%s_%d", 
                       DOWNLOAD_FOLDER, filename, counter++);
            }
        }
        
        rename(part_file, final_file);
        time_t end_time = time(NULL);
        double total_time = difftime(end_time, start_time);
        speed = total_time > 0 ? filesize / total_time : 0;
        
        show_progress(filesize, filesize, speed);
        printf("\n[Successfully saved to: %s]\n", final_file);
        printf("[Size: %ld bytes, Speed: %.2f MB/s]\n", filesize, speed/(1024*1024));
    } else {
        printf("\n[Download incomplete - partial file saved as %s]\n", part_file);
        printf("[Received: %ld/%ld bytes]\n", total_received, filesize);
    }
    printf("You: ");
    fflush(stdout);
    fclose(fp);
}

void *receive_handler(void *arg) {
    char buffer[BUFFER_SIZE];
    int in_list = 0;

    while (server_running) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes = recv(sock, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) {
            printf("\nDisconnected from server.\n");
            exit(0);
        }
        buffer[bytes] = '\0';

        if (strncmp(buffer, "[FILE]", 6) == 0) {
            char visibility = buffer[6];
            handle_file_download(visibility, buffer + 7, bytes - 7);
        } 
        else if (strncmp(buffer, "[LISTSTART]", 11) == 0) {
            in_list = 1;
            printf("\nServer files:\n");
        }
        else if (strncmp(buffer, "[LISTEND]", 9) == 0) {
            in_list = 0;
            printf("\nYou: ");
            fflush(stdout);
        }
        else if (in_list) {
            printf("%s", buffer);
        }
        // else {
        //     printf("\n%s\nYou: ", buffer);
        //     fflush(stdout);
        // }
    }
    return NULL;
}

void *send_handler(void *arg) {
    char buffer[BUFFER_SIZE];

    while (server_running) {
        printf("You: ");
        if (!fgets(buffer, BUFFER_SIZE, stdin)) {
            perror("Input error");
            break;
        }
        buffer[strcspn(buffer, "\n")] = '\0';

        if (strncmp(buffer, "/upload ", 8) == 0) {
            char *filename = buffer + 8;
            FILE *fp = fopen(filename, "rb");
            if (!fp) {
                perror("File open failed");
                continue;
            }

            printf("Set file visibility:\n");
            printf("1. Public (anyone can download)\n");
            printf("2. Private (requires access code)\n");
            printf("Choice: ");
            
            char visibility_choice[10];
            if (!fgets(visibility_choice, sizeof(visibility_choice), stdin)) {
                perror("Input error");
                fclose(fp);
                continue;
            }
            visibility_choice[strcspn(visibility_choice, "\n")] = '\0';
            
            char visibility = 'U';
            char access_code[MAX_CODE_LEN] = {0};
            
            if (strcmp(visibility_choice, "1") == 0) {
                visibility = 'P';
            } 
            else if (strcmp(visibility_choice, "2") == 0) {
                visibility = 'R';
                printf("Set access code (max %d chars): ", MAX_CODE_LEN);
                if (!fgets(access_code, sizeof(access_code), stdin)) {
                    perror("Input error");
                    fclose(fp);
                    continue;
                }
                access_code[strcspn(access_code, "\n")] = '\0';
                
                if (strlen(access_code) == 0) {
                    printf("Access code cannot be empty\n");
                    fclose(fp);
                    continue;
                }
            } 
            else {
                printf("Invalid choice. Using default visibility.\n");
            }

            if (send(sock, "[FILE]", 6, 0) <= 0) {
                perror("File header send failed");
                fclose(fp);
                continue;
            }

            if (send(sock, &visibility, 1, 0) <= 0) {
                perror("Visibility send failed");
                fclose(fp);
                continue;
            }

            if (visibility == 'R') {
                if (send(sock, access_code, strlen(access_code), 0) <= 0) {
                    perror("Access code send failed");
                    fclose(fp);
                    continue;
                }
            }

            int filename_len = strlen(filename);
            int net_filename_len = htonl(filename_len);
            if (send(sock, &net_filename_len, sizeof(net_filename_len), 0) <= 0) {
                perror("Filename length send failed");
                fclose(fp);
                continue;
            }
            if (send(sock, filename, filename_len, 0) <= 0) {
                perror("Filename send failed");
                fclose(fp);
                continue;
            }

            fseek(fp, 0, SEEK_END);
            long filesize = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            
            long net_filesize = htonl(filesize);
            if (send(sock, &net_filesize, sizeof(net_filesize), 0) <= 0) {
                perror("File size send failed");
                fclose(fp);
                continue;
            }

            printf("Uploading %s (%ld bytes)...\n", filename, filesize);
            
            time_t start_time = time(NULL);
            long total_sent = 0;
            time_t last_update = start_time;
            long last_bytes = 0;
            double speed = 0;

            int bytes;
            while ((bytes = fread(buffer, 1, BUFFER_SIZE, fp)) > 0 && server_running) {
                if (send(sock, buffer, bytes, 0) <= 0) {
                    perror("File content send failed");
                    break;
                }
                total_sent += bytes;
                
                time_t now = time(NULL);
                if (now - last_update >= 1) {
                    speed = (total_sent - last_bytes) / (double)(now - last_update);
                    show_progress(total_sent, filesize, speed);
                    last_update = now;
                    last_bytes = total_sent;
                }
            }

            time_t end_time = time(NULL);
            double total_time = difftime(end_time, start_time);
            speed = total_time > 0 ? filesize / total_time : 0;
            
            show_progress(filesize, filesize, speed);
            printf("\n[Uploaded file (%ld bytes, %.2f MB/s), Visibility: %s]\n", 
                   filesize, speed/(1024*1024),
                  visibility == 'P' ? "Public" : 
                  (visibility == 'R' ? "Private" : "User-only"));
            
            fclose(fp);
        } 
        else if (strncmp(buffer, "/download ", 10) == 0) {
            char filename[MAX_FILENAME_LEN];
            strncpy(filename, buffer + 10, MAX_FILENAME_LEN - 1);
            filename[strcspn(filename, "\n")] = '\0';

            char part_file[PATH_MAX];
            snprintf(part_file, sizeof(part_file), "%s/%s%s", 
                    DOWNLOAD_FOLDER, filename, PARTIAL_EXT);
            
            long offset = 0;
            struct stat st;
            if (stat(part_file, &st) == 0) {
                offset = st.st_size;
            }

            char command[BUFFER_SIZE];
            snprintf(command, sizeof(command), "/download %s %ld", filename, offset);
            if (send(sock, command, strlen(command), 0) <= 0) {
                perror("Download request failed");
            }
        } 
        else if (strcmp(buffer, "/list") == 0) {
            if (send(sock, "/list", 5, 0) <= 0) {
                perror("List request send failed");
            }
        } 
        else if (strcmp(buffer, "/locallist") == 0) {
            list_files();
        } 
        else if (strncmp(buffer, "/help", 5) == 0) {
            printf("\nAvailable commands:\n");
            printf("/upload <filename> - Upload a file to server\n");
            printf("/download <filename> - Download a file from server\n");
            printf("/list - List files available on server\n");
            printf("/locallist - List files in current directory\n");
            printf("/help - Show this help message\n");
            printf("\nFile visibility options when uploading:\n");
            printf("- Public: Anyone can download\n");
            printf("- Private: Requires access code to download\n");
        } 
        else if (strlen(buffer) > 0) {
            if (send(sock, buffer, strlen(buffer), 0) <= 0) {
                perror("Message send failed");
                break;
            }
        }
    }
    return NULL;
}

int authenticate() {
    char buffer[BUFFER_SIZE];
    char username[MAX_USERNAME_LEN];
    char password[MAX_PASSWORD_LEN];

    while (1) {
        printf("\n1. Login\n2. Register\nChoice: ");
        if (!fgets(buffer, sizeof(buffer), stdin)) {
            perror("Input error");
            return 0;
        }
        buffer[strcspn(buffer, "\n")] = 0;

        if (strcmp(buffer, "1") != 0 && strcmp(buffer, "2") != 0) {
            printf("Invalid choice. Please enter 1 or 2.\n");
            continue;
        }

        printf("Username: ");
        if (!fgets(username, sizeof(username), stdin)) {
            perror("Input error");
            return 0;
        }
        username[strcspn(username, "\n")] = 0;

        printf("Password: ");
        if (!fgets(password, sizeof(password), stdin)) {
            perror("Input error");
            return 0;
        }
        password[strcspn(password, "\n")] = 0;

        snprintf(buffer, BUFFER_SIZE, "%s %s %s", 
                (strcmp(buffer, "1") == 0) ? "/login" : "/register", 
                username, password);

        if (send(sock, buffer, strlen(buffer), 0) <= 0) {
            perror("Authentication send failed");
            return 0;
        }

        memset(buffer, 0, BUFFER_SIZE);
        int bytes = recv(sock, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) {
            perror("Authentication response failed");
            return 0;
        }

        buffer[bytes] = '\0';
        printf("%s\n", buffer);

        if (strstr(buffer, "successfully") || strstr(buffer, "successful")) {
            return 1;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server_ip>\n", argv[0]);
        return 1;
    }

    struct sockaddr_in serv_addr;
    pthread_t recv_thread, send_thread;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return 1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, argv[1], &serv_addr.sin_addr) <= 0) {
        perror("Invalid address");
        close(sock);
        return 1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection failed");
        close(sock);
        return 1;
    }

    ensure_download_folder();

    printf("Connected to server at %s:%d\n", argv[1], PORT);
    printf("Type /help for available commands\n");
    printf("Downloaded files will be saved in: %s\n", DOWNLOAD_FOLDER);

    if (!authenticate()) {
        close(sock);
        return 1;
    }

    pthread_create(&recv_thread, NULL, receive_handler, NULL);
    pthread_create(&send_thread, NULL, send_handler, NULL);

    pthread_join(recv_thread, NULL);
    pthread_join(send_thread, NULL);

    close(sock);
    return 0;
}