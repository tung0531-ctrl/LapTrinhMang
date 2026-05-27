#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BACKLOG  16
#define BUF_SIZE 256

static int send_str(SOCKET fd, const char *msg) {
    int total = 0, len = (int)strlen(msg);
    while (total < len) {
        int n = send(fd, msg + total, len - total, 0);
        if (n == SOCKET_ERROR) return -1;
        total += n;
    }
    return 0;
}

static int recv_line(SOCKET fd, char *buf, int max) {
    int used = 0;
    while (used + 1 < max) {
        char ch;
        int n = recv(fd, &ch, 1, 0);
        if (n <= 0) return n;
        buf[used++] = ch;
        if (ch == '\n') break;
    }
    buf[used] = '\0';
    return used;
}

static void trim_line(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1]=='\r' || s[n-1]=='\n' || s[n-1]==' ')) s[--n] = '\0';
}

static int build_time_string(const char *fmt_key, char *out, size_t out_sz) {
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    const char *pat = NULL;
    if (!lt) return -1;
    if      (strcmp(fmt_key, "dd/mm/yyyy") == 0) pat = "%d/%m/%Y";
    else if (strcmp(fmt_key, "dd/mm/yy")   == 0) pat = "%d/%m/%y";
    else if (strcmp(fmt_key, "mm/dd/yyyy") == 0) pat = "%m/%d/%Y";
    else if (strcmp(fmt_key, "mm/dd/yy")   == 0) pat = "%m/%d/%y";
    else return -1;
    return strftime(out, out_sz, pat, lt) > 0 ? 0 : -1;
}

struct carg { SOCKET sock; };

static DWORD WINAPI client_thread(LPVOID param) {
    struct carg *ca = (struct carg *)param;
    SOCKET sock = ca->sock;
    char line[BUF_SIZE];
    free(ca);

    send_str(sock,
        "Time server san sang. Lenh: GET_TIME [format]\r\n"
        "Format ho tro: dd/mm/yyyy | dd/mm/yy | mm/dd/yyyy | mm/dd/yy\r\n");

    while (1) {
        int n = recv_line(sock, line, sizeof(line));
        if (n <= 0) break;
        trim_line(line);
        if (!line[0]) continue;

        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            send_str(sock, "Bye\r\n");
            break;
        }

        {
            char cmd[64], fmt[64];
            char result[64], resp[128];
            int cnt;
            fmt[0] = '\0';
            cnt = sscanf(line, "%63s %63s", cmd, fmt);

            if (cnt < 1 || strcmp(cmd, "GET_TIME") != 0) {
                send_str(sock, "ERR Lenh khong hop le. Dung: GET_TIME [format]\r\n");
                continue;
            }
            if (fmt[0] == '\0') strcpy(fmt, "dd/mm/yyyy");

            if (build_time_string(fmt, result, sizeof(result)) < 0) {
                send_str(sock, "ERR Format khong ho tro. Dung: dd/mm/yyyy | dd/mm/yy | mm/dd/yyyy | mm/dd/yy\r\n");
                continue;
            }
            snprintf(resp, sizeof(resp), "OK %s\r\n", result);
            send_str(sock, resp);
        }
    }

    closesocket(sock);
    return 0;
}

int main(int argc, char *argv[]) {
    WSADATA wsa;
    SOCKET listener;
    struct sockaddr_in addr;
    int port;

    if (argc != 2) { fprintf(stderr, "Usage: %s <port>\n", argv[0]); return 1; }
    port = atoi(argv[1]);
    if (port <= 0 || port > 65535) { fprintf(stderr, "Invalid port\n"); return 1; }

    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { fprintf(stderr, "WSAStartup failed\n"); return 1; }

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { fprintf(stderr, "socket() failed\n"); return 1; }
    { BOOL opt = TRUE; setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)); }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    if (bind(listener, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(listener, BACKLOG) == SOCKET_ERROR) {
        fprintf(stderr, "bind/listen failed\n"); return 1;
    }

    printf("Time server (multithread) lang nghe cong %d\n", port); fflush(stdout);

    while (1) {
        struct sockaddr_in ca; int cl = sizeof(ca);
        SOCKET csock = accept(listener, (struct sockaddr*)&ca, &cl);
        struct carg *arg;
        HANDLE t;

        if (csock == INVALID_SOCKET) continue;

        arg = (struct carg *)malloc(sizeof(struct carg));
        if (!arg) { closesocket(csock); continue; }
        arg->sock = csock;

        t = CreateThread(NULL, 0, client_thread, arg, 0, NULL);
        if (!t) { free(arg); closesocket(csock); }
        else CloseHandle(t);
    }
}
