#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BACKLOG      16
#define MAX_CLIENTS  64
#define BUF_SIZE     2048
#define NAME_SIZE    64

struct client_entry {
    int    active;
    SOCKET sock;
    char   id[NAME_SIZE];
    char   name[NAME_SIZE];
    CRITICAL_SECTION wlock;   /* serialise writes to this socket */
};

static struct client_entry clients[MAX_CLIENTS];
static CRITICAL_SECTION    g_lock;

/* ---- helpers ---- */

static int send_all(SOCKET fd, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(fd, buf + sent, len - sent, 0);
        if (n == SOCKET_ERROR) return -1;
        sent += n;
    }
    return 0;
}

static int send_str(SOCKET fd, const char *msg) {
    return send_all(fd, msg, (int)strlen(msg));
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

static void trim_crlf(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1]=='\r' || s[n-1]=='\n')) s[--n] = '\0';
}

static int is_token(const char *s) {
    if (!s || !*s) return 0;
    for (; *s; s++) if ((unsigned char)*s <= ' ') return 0;
    return 1;
}

/* ---- broadcast (holds g_lock only briefly to copy slot list) ---- */

static void broadcast_except(SOCKET from, const char *msg) {
    int i, n = 0, slots[MAX_CLIENTS];
    int len = (int)strlen(msg);

    EnterCriticalSection(&g_lock);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].id[0] && clients[i].sock != from)
            slots[n++] = i;
    }
    LeaveCriticalSection(&g_lock);

    for (i = 0; i < n; i++) {
        int s = slots[i];
        EnterCriticalSection(&clients[s].wlock);
        if (clients[s].active && clients[s].id[0])
            send_all(clients[s].sock, msg, len);
        LeaveCriticalSection(&clients[s].wlock);
    }
}

/* ---- client thread ---- */

struct carg { int slot; SOCKET sock; };

static DWORD WINAPI client_thread(LPVOID param) {
    struct carg *ca = (struct carg *)param;
    int slot = ca->slot;
    SOCKET sock = ca->sock;
    char buf[BUF_SIZE];
    char msg[BUF_SIZE + 128];
    free(ca);

    /* registration: expect "id:name" */
    send_str(sock, "Dang ky (cu phap: id:ten):\r\n> ");
    while (1) {
        int n = recv_line(sock, buf, sizeof(buf));
        if (n <= 0) goto done;
        trim_crlf(buf);
        if (!buf[0]) { send_str(sock, "> "); continue; }

        {
            int dup = 0, i;
            char *sep  = strchr(buf, ':');
            char *id, *name;
            if (!sep) { send_str(sock, "Sai cu phap (id:ten). Thu lai:\r\n> "); continue; }
            *sep = '\0';
            id   = buf;
            name = sep + 1;
            while (*name == ' ' || *name == '\t') name++;
            trim_crlf(id); trim_crlf(name);

            if (!is_token(id) || !is_token(name)) {
                send_str(sock, "id/ten khong duoc chua khoang trang. Thu lai:\r\n> ");
                continue;
            }

            EnterCriticalSection(&g_lock);
            for (i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].active && clients[i].id[0] &&
                    strcmp(clients[i].id, id) == 0) { dup = 1; break; }
            }
            if (!dup) {
                strncpy(clients[slot].id,   id,   NAME_SIZE - 1);
                strncpy(clients[slot].name, name, NAME_SIZE - 1);
            }
            LeaveCriticalSection(&g_lock);

            if (dup) { send_str(sock, "ID da ton tai. Thu lai:\r\n> "); continue; }

            snprintf(msg, sizeof(msg), "Chao mung %s (%s)!\r\n", id, name);
            send_str(sock, msg);
            snprintf(msg, sizeof(msg), "[Server] %s (%s) vao phong chat\r\n", id, name);
            broadcast_except(sock, msg);
            printf("[+] %s (%s)\n", id, name); fflush(stdout);
            break;
        }
    }

    /* chat loop */
    while (1) {
        int n = recv_line(sock, buf, sizeof(buf));
        if (n <= 0) break;
        trim_crlf(buf);
        if (!buf[0]) continue;

        {
            SYSTEMTIME st; GetLocalTime(&st);
            snprintf(msg, sizeof(msg), "[%02d:%02d:%02d] %s: %s\r\n",
                     st.wHour, st.wMinute, st.wSecond, clients[slot].id, buf);
            printf("%s", msg); fflush(stdout);
            broadcast_except(sock, msg);
        }
    }

done:
    {
        char notice[256]; notice[0] = '\0';
        EnterCriticalSection(&g_lock);
        if (clients[slot].id[0])
            snprintf(notice, sizeof(notice), "[Server] %s (%s) roi phong chat\r\n",
                     clients[slot].id, clients[slot].name);
        clients[slot].active = 0;
        clients[slot].id[0]  = '\0';
        LeaveCriticalSection(&g_lock);
        if (notice[0]) { broadcast_except(sock, notice); printf("%s", notice); fflush(stdout); }
    }
    closesocket(sock);
    return 0;
}

/* ---- main ---- */

int main(int argc, char *argv[]) {
    WSADATA wsa;
    SOCKET listener;
    struct sockaddr_in addr;
    int port, i;

    if (argc != 2) { fprintf(stderr, "Usage: %s <port>\n", argv[0]); return 1; }
    port = atoi(argv[1]);
    if (port <= 0 || port > 65535) { fprintf(stderr, "Invalid port\n"); return 1; }

    InitializeCriticalSection(&g_lock);
    memset(clients, 0, sizeof(clients));
    for (i = 0; i < MAX_CLIENTS; i++)
        InitializeCriticalSection(&clients[i].wlock);

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

    printf("Chat server (multithread) lang nghe cong %d, toi da %d clients\n", port, MAX_CLIENTS);
    fflush(stdout);

    while (1) {
        struct sockaddr_in ca; int cl = sizeof(ca);
        SOCKET csock = accept(listener, (struct sockaddr*)&ca, &cl);
        int slot = -1;
        struct carg *arg;
        HANDLE t;

        if (csock == INVALID_SOCKET) continue;

        EnterCriticalSection(&g_lock);
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].active) {
                slot = i;
                clients[i].active = 1;
                clients[i].sock   = csock;
                clients[i].id[0]  = '\0';
                break;
            }
        }
        LeaveCriticalSection(&g_lock);

        if (slot < 0) {
            send(csock, "Server day, thu lai sau\r\n", 24, 0);
            closesocket(csock); continue;
        }

        arg = (struct carg *)malloc(sizeof(struct carg));
        if (!arg) { clients[slot].active = 0; closesocket(csock); continue; }
        arg->slot = slot; arg->sock = csock;

        t = CreateThread(NULL, 0, client_thread, arg, 0, NULL);
        if (!t) { free(arg); clients[slot].active = 0; closesocket(csock); }
        else CloseHandle(t);
    }
}
