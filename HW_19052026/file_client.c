#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_SIZE 4096
#define MAX_LINE 1024

static int send_all(SOCKET sockfd, const void *buffer, size_t length) {
    size_t sent_total = 0;
    const char *data = (const char *)buffer;

    while (sent_total < length) {
        int sent = send(sockfd, data + sent_total, (int)(length - sent_total), 0);
        if (sent == SOCKET_ERROR) {
            return -1;
        }
        sent_total += (size_t)sent;
    }

    return 0;
}

static int recv_line(SOCKET sockfd, char *buffer, size_t max_len) {
    size_t used = 0;

    if (max_len == 0) {
        return -1;
    }

    while (used + 1 < max_len) {
        char ch;
        int n = recv(sockfd, &ch, 1, 0);

        if (n == SOCKET_ERROR) {
            return -1;
        }
        if (n == 0) {
            if (used == 0) {
                return 0;
            }
            break;
        }

        buffer[used++] = ch;
        if (ch == '\n') {
            break;
        }
    }

    buffer[used] = '\0';
    return (int)used;
}

static void trim_crlf(char *text) {
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\r' || text[len - 1] == '\n')) {
        text[len - 1] = '\0';
        len--;
    }
}

static int receive_file_list(SOCKET sockfd) {
    char line[MAX_LINE];
    int n;

    n = recv_line(sockfd, line, sizeof(line));
    if (n <= 0) {
        fprintf(stderr, "Cannot receive response from server\n");
        return -1;
    }

    trim_crlf(line);
    if (strncmp(line, "ERROR", 5) == 0) {
        printf("Server: %s\n", line);
        return -1;
    }

    if (strncmp(line, "OK", 2) != 0) {
        fprintf(stderr, "Invalid list header: %s\n", line);
        return -1;
    }

    printf("Danh sach file tren server:\n");
    while (1) {
        n = recv_line(sockfd, line, sizeof(line));
        if (n <= 0) {
            fprintf(stderr, "Connection closed while receiving list\n");
            return -1;
        }

        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) {
            break;
        }

        trim_crlf(line);
        {
            /* format: filename SIZE - find last space */
            char *last_space = strrchr(line, ' ');
            if (last_space != NULL) {
                long long sz = _atoi64(last_space + 1);
                *last_space = '\0';
                printf("- %s (%lld bytes)\n", line, sz);
            } else {
                printf("- %s\n", line);
            }
        }
    }

    return 0;
}

static int request_and_download(SOCKET sockfd, const char *save_path) {
    char filename[MAX_LINE];

    while (1) {
        char response[MAX_LINE];
        int n;

        printf("Nhap ten file can tai: ");
        fflush(stdout);

        if (fgets(filename, sizeof(filename), stdin) == NULL) {
            return -1;
        }

        trim_crlf(filename);
        if (filename[0] == '\0') {
            continue;
        }

        {
            char send_line[MAX_LINE + 4];
            snprintf(send_line, sizeof(send_line), "%s\r\n", filename);
            if (send_all(sockfd, send_line, strlen(send_line)) < 0) {
                fprintf(stderr, "send() failed\n");
                return -1;
            }
        }

        n = recv_line(sockfd, response, sizeof(response));
        if (n <= 0) {
            fprintf(stderr, "Server closed connection\n");
            return -1;
        }

        trim_crlf(response);
        if (strncmp(response, "ERROR", 5) == 0) {
            printf("Server: %s\n", response);
            continue;
        }

        if (strncmp(response, "OK ", 3) == 0) {
            long long file_size = _atoi64(response + 3);
            FILE *fp;
            long long remaining;
            const char *target_path = save_path;

            if (target_path == NULL || target_path[0] == '\0') {
                target_path = filename;
            }

            if (file_size < 0) {
                fprintf(stderr, "Invalid file size from server\n");
                return -1;
            }

            {
                FILE *check_fp = fopen(target_path, "rb");
                if (check_fp != NULL) {
                    fclose(check_fp);
                    printf("Canh bao: file '%s' da ton tai, se bi ghi de.\n", target_path);
                }
            }

            fp = fopen(target_path, "wb");
            if (fp == NULL) {
                fprintf(stderr, "fopen() failed\n");
                return -1;
            }

            remaining = file_size;
            while (remaining > 0) {
                char buffer[BUFFER_SIZE];
                int chunk = (remaining > (long long)sizeof(buffer)) ? (int)sizeof(buffer) : (int)remaining;
                int received = recv(sockfd, buffer, chunk, 0);

                if (received == SOCKET_ERROR) {
                    fprintf(stderr, "recv() failed\n");
                    fclose(fp);
                    return -1;
                }
                if (received == 0) {
                    fprintf(stderr, "Unexpected EOF while receiving file\n");
                    fclose(fp);
                    return -1;
                }

                if (fwrite(buffer, 1, (size_t)received, fp) != (size_t)received) {
                    fprintf(stderr, "fwrite() failed\n");
                    fclose(fp);
                    return -1;
                }

                remaining -= received;
            }

            fclose(fp);
            printf("Da tai file va luu vao: %s\n", target_path);
            return 0;
        }

        fprintf(stderr, "Invalid response from server: %s\n", response);
        return -1;
    }
}

int main(int argc, char *argv[]) {
    WSADATA wsa_data;
    const char *server_ip;
    int port;
    const char *save_path;
    SOCKET sockfd;
    struct sockaddr_in server_addr;

    if (argc != 3 && argc != 4) {
        fprintf(stderr, "Usage: %s <server_ip> <port> [output_file]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return EXIT_FAILURE;
    }

    server_ip = argv[1];
    port = atoi(argv[2]);
    save_path = (argc == 4) ? argv[3] : NULL;

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[2]);
        WSACleanup();
        return EXIT_FAILURE;
    }

    sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed\n");
        WSACleanup();
        return EXIT_FAILURE;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((unsigned short)port);

    if (InetPtonA(AF_INET, server_ip, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", server_ip);
        closesocket(sockfd);
        WSACleanup();
        return EXIT_FAILURE;
    }

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "connect() failed\n");
        closesocket(sockfd);
        WSACleanup();
        return EXIT_FAILURE;
    }

    if (receive_file_list(sockfd) < 0) {
        closesocket(sockfd);
        WSACleanup();
        return EXIT_FAILURE;
    }

    if (request_and_download(sockfd, save_path) < 0) {
        closesocket(sockfd);
        WSACleanup();
        return EXIT_FAILURE;
    }

    closesocket(sockfd);
    WSACleanup();
    return EXIT_SUCCESS;
}
