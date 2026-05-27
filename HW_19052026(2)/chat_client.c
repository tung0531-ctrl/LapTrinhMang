#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF_SIZE 2048

static volatile LONG running = 1;

static int send_all(SOCKET fd, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(fd, buf + sent, len - sent, 0);
        if (n == SOCKET_ERROR) return -1;
        sent += n;
    }
    return 0;
}

static DWORD WINAPI recv_thread(LPVOID param) {
    SOCKET sock = (SOCKET)param;
    char buf[BUF_SIZE];
    while (1) {
        int n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        printf("%s", buf);
        fflush(stdout);
    }
    InterlockedExchange(&running, 0);
    return 0;
}

int main(int argc, char *argv[]) {
    WSADATA wsa;
    SOCKET sock;
    struct sockaddr_in addr;
    HANDLE rt;
    char buf[BUF_SIZE];
    int port;

    if (argc != 3) { fprintf(stderr, "Usage: %s <ip> <port>\n", argv[0]); return 1; }
    port = atoi(argv[2]);

    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { fprintf(stderr, "WSAStartup failed\n"); return 1; }

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) { fprintf(stderr, "socket() failed\n"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);
    if (InetPtonA(AF_INET, argv[1], &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IP: %s\n", argv[1]); return 1;
    }

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "connect() failed\n"); return 1;
    }

    rt = CreateThread(NULL, 0, recv_thread, (LPVOID)sock, 0, NULL);
    if (!rt) { fprintf(stderr, "CreateThread failed\n"); return 1; }

    while (running && fgets(buf, sizeof(buf), stdin)) {
        /* strip \n and send with \r\n */
        int len = (int)strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[--len] = '\0';
        buf[len] = '\r'; buf[len+1] = '\n'; buf[len+2] = '\0';
        if (send_all(sock, buf, len + 2) < 0) break;
    }

    shutdown(sock, SD_BOTH);
    WaitForSingleObject(rt, 3000);
    CloseHandle(rt);
    closesocket(sock);
    WSACleanup();
    return 0;
}
