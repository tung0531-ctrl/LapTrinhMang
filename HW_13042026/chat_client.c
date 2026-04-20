#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
typedef int io_result_t;
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int socket_t;
typedef ssize_t io_result_t;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define close_socket close
#endif

#define BUFFER_SIZE 2048

static volatile int keep_running = 1;

static int initialize_socket_system(void) {
#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return -1;
    }
#endif
    return 0;
}

static void cleanup_socket_system(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

static int parse_ipv4_address(const char *text, struct in_addr *address) {
#ifdef _WIN32
    unsigned long value = inet_addr(text);
    if (value == INADDR_NONE && strcmp(text, "255.255.255.255") != 0) {
        return 0;
    }

    address->s_addr = value;
    return 1;
#else
    return inet_pton(AF_INET, text, address) == 1;
#endif
}

static int send_all(socket_t sockfd, const char *buffer, size_t length) {
    size_t sent_total = 0;

    while (sent_total < length) {
        io_result_t sent = send(sockfd, buffer + sent_total, (int)(length - sent_total), 0);
        if (sent < 0) {
#ifndef _WIN32
            if (errno == EINTR) {
                continue;
            }
#endif
            return -1;
        }
        sent_total += (size_t)sent;
    }

    return 0;
}

#ifdef _WIN32
static DWORD WINAPI receive_messages(LPVOID argument)
#else
static void *receive_messages(void *argument)
#endif
{
    socket_t sockfd = *(socket_t *)argument;

    while (keep_running) {
        char buffer[BUFFER_SIZE];
        io_result_t received = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
        if (received < 0) {
            perror("recv() failed");
            keep_running = 0;
            break;
        }
        if (received == 0) {
            printf("Server da dong ket noi\n");
            keep_running = 0;
            break;
        }

        buffer[received] = '\0';
        printf("%s", buffer);
        fflush(stdout);
    }

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <ip> <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    {
        int port = atoi(argv[2]);
        socket_t client;
        struct sockaddr_in server_addr;

        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[2]);
            return EXIT_FAILURE;
        }

        if (initialize_socket_system() < 0) {
            fprintf(stderr, "Khong khoi tao duoc socket system\n");
            return EXIT_FAILURE;
        }

        client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (client == INVALID_SOCKET) {
            perror("socket() failed");
            cleanup_socket_system();
            return EXIT_FAILURE;
        }

        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons((unsigned short)port);

        if (!parse_ipv4_address(argv[1], &server_addr.sin_addr)) {
            fprintf(stderr, "Invalid IPv4 address: %s\n", argv[1]);
            close_socket(client);
            cleanup_socket_system();
            return EXIT_FAILURE;
        }

        if (connect(client, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            perror("connect() failed");
            close_socket(client);
            cleanup_socket_system();
            return EXIT_FAILURE;
        }

        printf("Da ket noi chat server %s:%d\n", argv[1], port);

#ifdef _WIN32
        {
            HANDLE receiver = CreateThread(NULL, 0, receive_messages, &client, 0, NULL);
            if (receiver == NULL) {
                fprintf(stderr, "Khong tao duoc luong nhan du lieu\n");
                close_socket(client);
                cleanup_socket_system();
                return EXIT_FAILURE;
            }

            while (keep_running) {
                char buffer[BUFFER_SIZE];
                if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
                    keep_running = 0;
                    break;
                }

                if (send_all(client, buffer, strlen(buffer)) < 0) {
                    perror("send() failed");
                    keep_running = 0;
                    break;
                }
            }

            shutdown(client, SD_BOTH);
            WaitForSingleObject(receiver, INFINITE);
            CloseHandle(receiver);
        }
#else
        {
            pthread_t receiver;
            if (pthread_create(&receiver, NULL, receive_messages, &client) != 0) {
                perror("pthread_create() failed");
                close_socket(client);
                cleanup_socket_system();
                return EXIT_FAILURE;
            }

            while (keep_running) {
                char buffer[BUFFER_SIZE];
                if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
                    keep_running = 0;
                    break;
                }

                if (send_all(client, buffer, strlen(buffer)) < 0) {
                    perror("send() failed");
                    keep_running = 0;
                    break;
                }
            }

            shutdown(client, SHUT_RDWR);
            pthread_join(receiver, NULL);
        }
#endif

        close_socket(client);
        cleanup_socket_system();
    }

    return EXIT_SUCCESS;
}