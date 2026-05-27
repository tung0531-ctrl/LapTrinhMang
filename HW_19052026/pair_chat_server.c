#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BACKLOG 16
#define BUFFER_SIZE 2048

struct waiting_queue {
    int has_waiting;
    SOCKET waiting_sock;
    CRITICAL_SECTION lock;
};

struct chat_pair {
    SOCKET client_a;
    SOCKET client_b;
    CRITICAL_SECTION close_lock;
    int closed;
};

struct relay_arg {
    struct chat_pair *pair;
    SOCKET from_sock;
    SOCKET to_sock;
};

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

static void close_pair_once(struct chat_pair *pair) {
    EnterCriticalSection(&pair->close_lock);
    if (!pair->closed) {
        pair->closed = 1;
        shutdown(pair->client_a, SD_BOTH);
        shutdown(pair->client_b, SD_BOTH);
        closesocket(pair->client_a);
        closesocket(pair->client_b);
    }
    LeaveCriticalSection(&pair->close_lock);
}

static DWORD WINAPI relay_thread(LPVOID arg) {
    struct relay_arg *relay = (struct relay_arg *)arg;
    char buffer[BUFFER_SIZE];

    while (1) {
        int n = recv(relay->from_sock, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;
        }

        if (send_all(relay->to_sock, buffer, (size_t)n) < 0) {
            break;
        }
    }

    close_pair_once(relay->pair);
    return 0;
}

static DWORD WINAPI pair_session_thread(LPVOID arg) {
    struct chat_pair *pair = (struct chat_pair *)arg;
    HANDLE t1;
    HANDLE t2;
    struct relay_arg *r1;
    struct relay_arg *r2;

    r1 = (struct relay_arg *)malloc(sizeof(struct relay_arg));
    r2 = (struct relay_arg *)malloc(sizeof(struct relay_arg));
    if (r1 == NULL || r2 == NULL) {
        free(r1);
        free(r2);
        close_pair_once(pair);
        DeleteCriticalSection(&pair->close_lock);
        free(pair);
        return 0;
    }

    r1->pair = pair;
    r1->from_sock = pair->client_a;
    r1->to_sock = pair->client_b;

    r2->pair = pair;
    r2->from_sock = pair->client_b;
    r2->to_sock = pair->client_a;

    send_all(pair->client_a, "Matched. Start chatting...\n", strlen("Matched. Start chatting...\n"));
    send_all(pair->client_b, "Matched. Start chatting...\n", strlen("Matched. Start chatting...\n"));

    t1 = CreateThread(NULL, 0, relay_thread, r1, 0, NULL);
    t2 = CreateThread(NULL, 0, relay_thread, r2, 0, NULL);
    if (t1 == NULL || t2 == NULL) {
        if (t1 != NULL) {
            WaitForSingleObject(t1, INFINITE);
            CloseHandle(t1);
        }
        if (t2 != NULL) {
            WaitForSingleObject(t2, INFINITE);
            CloseHandle(t2);
        }
        free(r1);
        free(r2);
        close_pair_once(pair);
        DeleteCriticalSection(&pair->close_lock);
        free(pair);
        return 0;
    }

    WaitForSingleObject(t1, INFINITE);
    WaitForSingleObject(t2, INFINITE);

    CloseHandle(t1);
    CloseHandle(t2);
    free(r1);
    free(r2);

    DeleteCriticalSection(&pair->close_lock);
    free(pair);
    return 0;
}

int main(int argc, char *argv[]) {
    WSADATA wsa_data;
    SOCKET listen_sock;
    int port;
    struct sockaddr_in server_addr;
    struct waiting_queue queue;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return EXIT_FAILURE;
    }

    port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        WSACleanup();
        return EXIT_FAILURE;
    }

    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed\n");
        WSACleanup();
        return EXIT_FAILURE;
    }

    {
        BOOL opt = TRUE;
        if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt)) == SOCKET_ERROR) {
            fprintf(stderr, "setsockopt() failed\n");
            closesocket(listen_sock);
            WSACleanup();
            return EXIT_FAILURE;
        }
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((unsigned short)port);

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind() failed\n");
        closesocket(listen_sock);
        WSACleanup();
        return EXIT_FAILURE;
    }

    if (listen(listen_sock, BACKLOG) == SOCKET_ERROR) {
        fprintf(stderr, "listen() failed\n");
        closesocket(listen_sock);
        WSACleanup();
        return EXIT_FAILURE;
    }

    queue.has_waiting = 0;
    queue.waiting_sock = INVALID_SOCKET;
    InitializeCriticalSection(&queue.lock);

    printf("Pair chat server listening on port %d\n", port);

    while (1) {
        SOCKET client_sock;
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);

        client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock == INVALID_SOCKET) {
            continue;
        }

        EnterCriticalSection(&queue.lock);
        if (!queue.has_waiting) {
            queue.has_waiting = 1;
            queue.waiting_sock = client_sock;
            LeaveCriticalSection(&queue.lock);
            send_all(client_sock, "Waiting for partner...\n", strlen("Waiting for partner...\n"));
        } else {
            SOCKET peer_sock = queue.waiting_sock;
            struct chat_pair *pair = (struct chat_pair *)malloc(sizeof(struct chat_pair));
            HANDLE session_thread;

            queue.has_waiting = 0;
            queue.waiting_sock = INVALID_SOCKET;
            LeaveCriticalSection(&queue.lock);

            if (pair == NULL) {
                closesocket(peer_sock);
                closesocket(client_sock);
                continue;
            }

            pair->client_a = peer_sock;
            pair->client_b = client_sock;
            pair->closed = 0;
            InitializeCriticalSection(&pair->close_lock);

            session_thread = CreateThread(NULL, 0, pair_session_thread, pair, 0, NULL);
            if (session_thread == NULL) {
                closesocket(peer_sock);
                closesocket(client_sock);
                DeleteCriticalSection(&pair->close_lock);
                free(pair);
                continue;
            }

            CloseHandle(session_thread);
        }
    }

    DeleteCriticalSection(&queue.lock);
    closesocket(listen_sock);
    WSACleanup();
    return EXIT_SUCCESS;
}
