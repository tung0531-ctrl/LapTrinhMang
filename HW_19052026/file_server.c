#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BACKLOG 16
#define BUFFER_SIZE 4096
#define MAX_FILENAME 256
#define MAX_PATH_LEN 1024

/* ---- helpers ---- */

static int send_all(SOCKET fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        int n = send(fd, p + sent, (int)(len - sent), 0);
        if (n == SOCKET_ERROR) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int recv_line(SOCKET fd, char *buf, size_t max) {
    size_t used = 0;
    while (used + 1 < max) {
        char ch;
        int n = recv(fd, &ch, 1, 0);
        if (n == SOCKET_ERROR) return -1;
        if (n == 0) { if (used == 0) return 0; break; }
        buf[used++] = ch;
        if (ch == '\n') break;
    }
    buf[used] = '\0';
    return (int)used;
}

static void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1]=='\r'||s[n-1]=='\n')) s[--n] = '\0';
}

static int is_safe_filename(const char *name) {
    if (!name || !*name) return 0;
    if (strstr(name, "..")) return 0;
    if (strchr(name, '/') || strchr(name, '\\')) return 0;
    return 1;
}

/* ---- file list ---- */

static int count_files(const char *dir) {
    WIN32_FIND_DATAA fd; HANDLE h; char pat[MAX_PATH_LEN]; int c = 0;
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (strcmp(fd.cFileName,".")==0||strcmp(fd.cFileName,"..")==0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        c++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return c;
}

static int send_names(SOCKET sock, const char *dir) {
    WIN32_FIND_DATAA fd; HANDLE h; char pat[MAX_PATH_LEN];
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        char line[MAX_FILENAME + 32];
        long long sz;
        if (strcmp(fd.cFileName,".")==0||strcmp(fd.cFileName,"..")==0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        sz = ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        snprintf(line, sizeof(line), "%s %lld\r\n", fd.cFileName, sz);
        if (send_all(sock, line, strlen(line)) < 0) { FindClose(h); return -1; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

static int send_file_list(SOCKET sock, const char *dir) {
    int cnt = count_files(dir);
    char hdr[64];
    if (cnt < 0) { const char *m="ERRORCannot open directory\r\n"; return send_all(sock,m,strlen(m)); }
    if (cnt == 0) { const char *m="ERRORNofilestodownload\r\n"; return send_all(sock,m,strlen(m)); }
    snprintf(hdr, sizeof(hdr), "OK %d\r\n", cnt);
    if (send_all(sock, hdr, strlen(hdr)) < 0) return -1;
    if (send_names(sock, dir) < 0) return -1;
    return send_all(sock, "\r\n", 2);
}

/* ---- file transfer ---- */

static int send_file(SOCKET sock, const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA fa; char hdr[64]; FILE *fp; char buf[BUFFER_SIZE];
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return -1;
    if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return -1;
    {
        long long sz = ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        snprintf(hdr, sizeof(hdr), "OK %lld\r\n", sz);
        if (send_all(sock, hdr, strlen(hdr)) < 0) return -1;
    }
    fp = fopen(path, "rb");
    if (!fp) return -1;
    while (1) {
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (n > 0 && send_all(sock, buf, n) < 0) { fclose(fp); return -1; }
        if (n < sizeof(buf)) { if (ferror(fp)) { fclose(fp); return -1; } break; }
    }
    fclose(fp);
    return 0;
}

/* ---- client handler (runs in a thread) ---- */

struct client_arg { SOCKET sock; char dir[MAX_PATH_LEN]; };

static DWORD WINAPI client_thread(LPVOID arg) {
    struct client_arg *ca = (struct client_arg *)arg;
    SOCKET sock = ca->sock;
    char dir[MAX_PATH_LEN];
    char line[MAX_FILENAME + 16];
    strncpy(dir, ca->dir, sizeof(dir)-1); dir[sizeof(dir)-1]='\0';
    free(ca);

    if (send_file_list(sock, dir) < 0) { closesocket(sock); return 0; }

    while (1) {
        int n = recv_line(sock, line, sizeof(line));
        if (n <= 0) break;
        trim_crlf(line);
        if (!is_safe_filename(line)) {
            const char *m = "ERRORInvalid filename\r\n";
            if (send_all(sock, m, strlen(m)) < 0) break;
            continue;
        }
        {
            char full[MAX_PATH_LEN]; long long sz; WIN32_FILE_ATTRIBUTE_DATA fa;
            snprintf(full, sizeof(full), "%s\\%s", dir, line);
            if (!GetFileAttributesExA(full, GetFileExInfoStandard, &fa) ||
                (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                const char *m = "ERRORFile not found\r\n";
                if (send_all(sock, m, strlen(m)) < 0) break;
                continue;
            }
            sz = ((long long)fa.nFileSizeHigh<<32)|fa.nFileSizeLow;
            if (send_file(sock, full) < 0) break;
            printf("Sent: %s (%lld bytes)\n", line, sz); fflush(stdout);
            break; /* one file per connection as per spec */
        }
    }

    closesocket(sock);
    return 0;
}

/* ---- main ---- */

int main(int argc, char *argv[]) {
    WSADATA wsa; SOCKET listen_sock; struct sockaddr_in addr;
    int port; const char *dir; DWORD attrs;

    if (argc != 3) { fprintf(stderr,"Usage: %s <port> <shared_dir>\n",argv[0]); return EXIT_FAILURE; }
    if (WSAStartup(MAKEWORD(2,2),&wsa)!=0) { fprintf(stderr,"WSAStartup failed\n"); return EXIT_FAILURE; }

    port = atoi(argv[1]); dir = argv[2];
    if (port<=0||port>65535) { fprintf(stderr,"Invalid port\n"); WSACleanup(); return EXIT_FAILURE; }

    attrs = GetFileAttributesA(dir);
    if (attrs==INVALID_FILE_ATTRIBUTES||(attrs&FILE_ATTRIBUTE_DIRECTORY)==0) {
        fprintf(stderr,"Invalid directory: %s\n",dir); WSACleanup(); return EXIT_FAILURE;
    }

    listen_sock = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if (listen_sock==INVALID_SOCKET) { fprintf(stderr,"socket() failed\n"); WSACleanup(); return EXIT_FAILURE; }
    { BOOL opt=TRUE; setsockopt(listen_sock,SOL_SOCKET,SO_REUSEADDR,(const char*)&opt,sizeof(opt)); }

    memset(&addr,0,sizeof(addr));
    addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_ANY); addr.sin_port=htons((unsigned short)port);
    if (bind(listen_sock,(struct sockaddr*)&addr,sizeof(addr))==SOCKET_ERROR) { fprintf(stderr,"bind() failed\n"); closesocket(listen_sock); WSACleanup(); return EXIT_FAILURE; }
    if (listen(listen_sock,BACKLOG)==SOCKET_ERROR) { fprintf(stderr,"listen() failed\n"); closesocket(listen_sock); WSACleanup(); return EXIT_FAILURE; }

    printf("File server listening on port %d, folder: %s\n",port,dir); fflush(stdout);

    while (1) {
        struct sockaddr_in ca; int cl=sizeof(ca);
        SOCKET csock = accept(listen_sock,(struct sockaddr*)&ca,&cl);
        if (csock==INVALID_SOCKET) continue;
        {
            struct client_arg *arg = (struct client_arg*)malloc(sizeof(struct client_arg));
            HANDLE t;
            if (!arg) { closesocket(csock); continue; }
            arg->sock = csock;
            strncpy(arg->dir, dir, sizeof(arg->dir)-1); arg->dir[sizeof(arg->dir)-1]='\0';
            t = CreateThread(NULL,0,client_thread,arg,0,NULL);
            if (!t) { free(arg); closesocket(csock); continue; }
            CloseHandle(t);
        }
    }

    closesocket(listen_sock); WSACleanup(); return EXIT_SUCCESS;
}
