#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_SIZE 2048

static volatile LONG keep_running = 1;

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

static DWORD WINAPI receive_thread(LPVOID arg) {
    SOCKET sockfd = *(SOCKET *)arg;

    while (InterlockedCompareExchange(&keep_running, 1, 1)) {
        char buffer[BUFFER_SIZE + 1];
        int n = recv(sockfd, buffer, BUFFER_SIZE, 0);
        if (n == SOCKET_ERROR) {
            fprintf(stderr, "recv() failed\n");
            InterlockedExchange(&keep_running, 0);
            break;
        }
        if (n == 0) {
            printf("Disconnected by server\n");
            InterlockedExchange(&keep_running, 0);
            break;
        }

        buffer[n] = '\0';
        printf("%s", buffer);
        fflush(stdout);
    }

    return 0;
}

int main(int argc, char *argv[]) {
    WSADATA wsa_data;
    const char *server_ip;
    int port;
    SOCKET sockfd;
    struct sockaddr_in server_addr;
    HANDLE receiver;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return EXIT_FAILURE;
    }

    server_ip = argv[1];
    port = atoi(argv[2]);

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

    printf("Connected to server %s:%d\n", server_ip, port);

    receiver = CreateThread(NULL, 0, receive_thread, &sockfd, 0, NULL);
    if (receiver == NULL) {
        fprintf(stderr, "CreateThread() failed\n");
        closesocket(sockfd);
        WSACleanup();
        return EXIT_FAILURE;
    }

    while (InterlockedCompareExchange(&keep_running, 1, 1)) {
        char line[BUFFER_SIZE];

        if (fgets(line, sizeof(line), stdin) == NULL) {
            InterlockedExchange(&keep_running, 0);
            break;
        }

        if (send_all(sockfd, line, strlen(line)) < 0) {
            fprintf(stderr, "send() failed\n");
            InterlockedExchange(&keep_running, 0);
            break;
        }
    }

    shutdown(sockfd, SD_BOTH);
    WaitForSingleObject(receiver, INFINITE);
    CloseHandle(receiver);

    closesocket(sockfd);
    WSACleanup();
    return EXIT_SUCCESS;
}
