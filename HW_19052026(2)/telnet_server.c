#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BACKLOG      16
#define BUF_SIZE     2048
#define FIELD_SIZE   128
#define MAX_ACCOUNTS 32
#define MAX_RETRIES  3

struct account { char user[FIELD_SIZE]; char pass[FIELD_SIZE]; };

static struct account accounts[MAX_ACCOUNTS];
static int num_accounts = 0;

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
        if (ch == '\r') continue;   /* skip CR from telnet clients */
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

static void load_accounts(const char *path) {
    FILE *fp = fopen(path, "r");
    char line[256];
    if (!fp) { fprintf(stderr, "Khong mo duoc %s\n", path); return; }
    while (num_accounts < MAX_ACCOUNTS && fgets(line, sizeof(line), fp)) {
        char u[FIELD_SIZE], p[FIELD_SIZE];
        if (sscanf(line, "%127s %127s", u, p) == 2) {
            strncpy(accounts[num_accounts].user, u, FIELD_SIZE - 1);
            strncpy(accounts[num_accounts].pass, p, FIELD_SIZE - 1);
            num_accounts++;
        }
    }
    fclose(fp);
    printf("Da tai %d tai khoan tu %s\n", num_accounts, path);
}

static int check_login(const char *user, const char *pass) {
    int i;
    for (i = 0; i < num_accounts; i++)
        if (strcmp(accounts[i].user, user) == 0 &&
            strcmp(accounts[i].pass, pass) == 0) return 1;
    return 0;
}

/* ---- command handler ---- */

static void handle_command(SOCKET fd, const char *username, const char *cmd) {
    char resp[BUF_SIZE];

    if (strcmp(cmd, "help") == 0) {
        send_str(fd,
            "Lenh ho tro:\r\n"
            "  help   - hien thi tro giup\r\n"
            "  whoami - ten dang nhap hien tai\r\n"
            "  date   - ngay gio he thong\r\n"
            "  pwd    - thu muc hien tai\r\n"
            "  ls     - liet ke file/thu muc\r\n"
            "  exit   - ngat ket noi\r\n");
    } else if (strcmp(cmd, "whoami") == 0) {
        snprintf(resp, sizeof(resp), "%s\r\n", username);
        send_str(fd, resp);
    } else if (strcmp(cmd, "date") == 0) {
        SYSTEMTIME st; GetLocalTime(&st);
        snprintf(resp, sizeof(resp), "%04d-%02d-%02d %02d:%02d:%02d\r\n",
                 st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);
        send_str(fd, resp);
    } else if (strcmp(cmd, "pwd") == 0) {
        char path[MAX_PATH];
        GetCurrentDirectoryA(MAX_PATH, path);
        snprintf(resp, sizeof(resp), "%s\r\n", path);
        send_str(fd, resp);
    } else if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "dir") == 0) {
        WIN32_FIND_DATAA fd_data;
        HANDLE h = FindFirstFileA(".\\*", &fd_data);
        if (h == INVALID_HANDLE_VALUE) {
            send_str(fd, "Loi khi lay danh sach file\r\n");
        } else {
            do {
                if (strcmp(fd_data.cFileName, ".") == 0 ||
                    strcmp(fd_data.cFileName, "..") == 0) continue;
                int isdir = (fd_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                snprintf(resp, sizeof(resp), "  %s%s\r\n",
                         fd_data.cFileName, isdir ? "/" : "");
                send_str(fd, resp);
            } while (FindNextFileA(h, &fd_data));
            FindClose(h);
        }
    } else {
        snprintf(resp, sizeof(resp), "Lenh khong hop le: '%s'. Goi 'help' de xem lenh.\r\n", cmd);
        send_str(fd, resp);
    }
}

/* ---- client thread ---- */

struct carg { SOCKET sock; };

static DWORD WINAPI client_thread(LPVOID param) {
    struct carg *ca = (struct carg *)param;
    SOCKET sock = ca->sock;
    char buf[BUF_SIZE];
    char username[FIELD_SIZE] = {0};
    int tries = 0;
    free(ca);

    /* login */
    while (tries < MAX_RETRIES) {
        send_str(sock, "Username: ");
        if (recv_line(sock, buf, sizeof(buf)) <= 0) goto done;
        trim_line(buf);
        if (!buf[0]) continue;
        strncpy(username, buf, FIELD_SIZE - 1);

        send_str(sock, "Password: ");
        if (recv_line(sock, buf, sizeof(buf)) <= 0) goto done;
        trim_line(buf);

        if (check_login(username, buf)) {
            char welcome[FIELD_SIZE + 32];
            snprintf(welcome, sizeof(welcome), "Chao mung, %s!\r\n", username);
            send_str(sock, welcome);
            break;
        }

        tries++;
        if (tries < MAX_RETRIES)
            send_str(sock, "Sai ten dang nhap hoac mat khau. Thu lai.\r\n");
        else
            send_str(sock, "Qua so lan thu. Ngat ket noi.\r\n");
    }
    if (tries >= MAX_RETRIES) goto done;

    printf("[+] '%s' da dang nhap\n", username); fflush(stdout);

    /* shell */
    while (1) {
        char prompt[FIELD_SIZE + 4];
        snprintf(prompt, sizeof(prompt), "%s> ", username);
        send_str(sock, prompt);

        if (recv_line(sock, buf, sizeof(buf)) <= 0) break;
        trim_line(buf);
        if (!buf[0]) continue;

        if (strcmp(buf, "exit") == 0 || strcmp(buf, "quit") == 0) {
            send_str(sock, "Bye!\r\n");
            break;
        }
        handle_command(sock, username, buf);
    }

done:
    printf("[-] '%s' ngat ket noi\n", username[0] ? username : "?"); fflush(stdout);
    closesocket(sock);
    return 0;
}

/* ---- main ---- */

int main(int argc, char *argv[]) {
    WSADATA wsa;
    SOCKET listener;
    struct sockaddr_in addr;
    int port;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <port> <accounts_file>\n", argv[0]); return 1;
    }
    port = atoi(argv[1]);
    if (port <= 0 || port > 65535) { fprintf(stderr, "Invalid port\n"); return 1; }

    load_accounts(argv[2]);

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

    printf("Telnet server (multithread) lang nghe cong %d\n", port); fflush(stdout);

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
