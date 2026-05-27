#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define DEFAULT_WORKERS  4
#define MAX_WORKERS      64
#define QUEUE_CAPACITY   256
#define BUF_SIZE         4096

/* ------------------------------------------------------------------ */
/* Work queue: main thread enqueues accepted sockets;                  */
/* pre-created worker threads dequeue and serve them.                  */
/* ------------------------------------------------------------------ */

static SOCKET           wq_buf[QUEUE_CAPACITY];
static int              wq_head  = 0;
static int              wq_tail  = 0;
static int              wq_count = 0;
static CRITICAL_SECTION wq_lock;
static HANDLE           wq_sem;   /* counting semaphore, value = item count */

static int wq_push(SOCKET sock) {
    EnterCriticalSection(&wq_lock);
    if (wq_count >= QUEUE_CAPACITY) {
        LeaveCriticalSection(&wq_lock);
        return -1;   /* queue full */
    }
    wq_buf[wq_tail] = sock;
    wq_tail = (wq_tail + 1) % QUEUE_CAPACITY;
    wq_count++;
    LeaveCriticalSection(&wq_lock);
    ReleaseSemaphore(wq_sem, 1, NULL);
    return 0;
}

static SOCKET wq_pop(void) {
    SOCKET sock;
    WaitForSingleObject(wq_sem, INFINITE);
    EnterCriticalSection(&wq_lock);
    sock = wq_buf[wq_head];
    wq_head = (wq_head + 1) % QUEUE_CAPACITY;
    wq_count--;
    LeaveCriticalSection(&wq_lock);
    return sock;
}

/* ------------------------------------------------------------------ */
/* HTTP helpers                                                        */
/* ------------------------------------------------------------------ */

static int send_all(SOCKET fd, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(fd, buf + sent, len - sent, 0);
        if (n == SOCKET_ERROR) return -1;
        sent += n;
    }
    return 0;
}

static const char *content_type_for(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".txt")  == 0) return "text/plain";
    if (strcmp(ext, ".css")  == 0) return "text/css";
    if (strcmp(ext, ".js")   == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    return "application/octet-stream";
}

static void send_simple(SOCKET fd, int code, const char *reason,
                         const char *ctype, const char *body) {
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\nConnection: close\r\n"
        "Content-Type: %s\r\nContent-Length: %zu\r\n\r\n",
        code, reason, ctype, strlen(body));
    send_all(fd, hdr, hlen);
    send_all(fd, body, (int)strlen(body));
}

static void serve_file(SOCKET fd, const char *path) {
    FILE *fp = fopen(path, "rb");
    char hdr[512], buf[BUF_SIZE];
    long fsz;
    int hlen;

    if (!fp) { send_simple(fd, 404, "Not Found", "text/plain", "404 Not Found\n"); return; }
    fseek(fp, 0, SEEK_END); fsz = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (fsz < 0) { fclose(fp); send_simple(fd, 500, "Internal Server Error", "text/plain", "500\n"); return; }

    hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nConnection: close\r\n"
        "Content-Type: %s\r\nContent-Length: %ld\r\n\r\n",
        content_type_for(path), fsz);
    send_all(fd, hdr, hlen);

    while (1) {
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (n > 0) send_all(fd, buf, (int)n);
        if (n < sizeof(buf)) break;
    }
    fclose(fp);
}

static int sanitize_path(const char *raw, char *out, size_t out_sz) {
    size_t i;
    if (!raw || !*raw || strcmp(raw, "/") == 0) { snprintf(out, out_sz, "index.html"); return 0; }
    if (raw[0] != '/') return -1;
    if (strstr(raw, "..")) return -1;
    for (i = 1; raw[i] && (i - 1) < out_sz - 1; i++) {
        char c = raw[i];
        if (c == '?') break;
        if (!(isalnum((unsigned char)c) || c == '/' || c == '.' || c == '_' || c == '-'))
            return -1;
        out[i - 1] = c;
    }
    out[i - 1] = '\0';
    if (out[0] == '\0') snprintf(out, out_sz, "index.html");
    return 0;
}

static void handle_request(SOCKET fd, int worker_id) {
    char buf[BUF_SIZE];
    char method[16], path[256], version[32], safe[256];
    int received;
    char *eol;

    received = recv(fd, buf, sizeof(buf) - 1, 0);
    if (received <= 0) return;
    buf[received] = '\0';

    eol = strstr(buf, "\r\n");
    if (eol) *eol = '\0';

    if (sscanf(buf, "%15s %255s %31s", method, path, version) != 3) {
        send_simple(fd, 400, "Bad Request", "text/plain", "400 Bad Request\n"); return;
    }
    if (strcmp(method, "GET") != 0) {
        send_simple(fd, 405, "Method Not Allowed", "text/plain", "405 Method Not Allowed\n"); return;
    }
    if (sanitize_path(path, safe, sizeof(safe)) < 0) {
        send_simple(fd, 400, "Bad Request", "text/plain", "400 Bad Request\n"); return;
    }

    printf("[Worker %d] GET %s -> %s\n", worker_id, path, safe); fflush(stdout);
    serve_file(fd, safe);
}

/* ------------------------------------------------------------------ */
/* Worker thread: waits on semaphore, pops socket, serves request      */
/* ------------------------------------------------------------------ */

struct warg { int id; };

static DWORD WINAPI worker_thread(LPVOID param) {
    struct warg *wa = (struct warg *)param;
    int id = wa->id;
    free(wa);
    while (1) {
        SOCKET csock = wq_pop();
        handle_request(csock, id);
        closesocket(csock);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    WSADATA wsa;
    SOCKET listener;
    struct sockaddr_in addr;
    int port, nworkers, i;

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <port> <webroot> [workers]\n", argv[0]); return 1;
    }
    port     = atoi(argv[1]);
    nworkers = (argc == 4) ? atoi(argv[3]) : DEFAULT_WORKERS;
    if (port <= 0 || port > 65535) { fprintf(stderr, "Invalid port\n"); return 1; }
    if (nworkers <= 0 || nworkers > MAX_WORKERS) nworkers = DEFAULT_WORKERS;

    if (!SetCurrentDirectoryA(argv[2])) {
        fprintf(stderr, "Khong chuyen duoc vao webroot: %s\n", argv[2]); return 1;
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { fprintf(stderr, "WSAStartup failed\n"); return 1; }

    InitializeCriticalSection(&wq_lock);
    wq_sem = CreateSemaphore(NULL, 0, QUEUE_CAPACITY, NULL);
    if (!wq_sem) { fprintf(stderr, "CreateSemaphore failed\n"); return 1; }

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { fprintf(stderr, "socket() failed\n"); return 1; }
    { BOOL opt = TRUE; setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)); }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    if (bind(listener, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(listener, 32) == SOCKET_ERROR) {
        fprintf(stderr, "bind/listen failed\n"); return 1;
    }

    /* pre-create worker threads */
    for (i = 0; i < nworkers; i++) {
        struct warg *wa = (struct warg *)malloc(sizeof(struct warg));
        HANDLE t;
        if (!wa) continue;
        wa->id = i + 1;
        t = CreateThread(NULL, 0, worker_thread, wa, 0, NULL);
        if (!t) free(wa); else CloseHandle(t);
    }

    printf("HTTP server (prethreading) cong %d | webroot: %s | %d workers\n",
           port, argv[2], nworkers);
    fflush(stdout);

    while (1) {
        struct sockaddr_in ca; int cl = sizeof(ca);
        SOCKET csock = accept(listener, (struct sockaddr*)&ca, &cl);
        if (csock == INVALID_SOCKET) continue;
        if (wq_push(csock) < 0) {
            send_simple(csock, 503, "Service Unavailable", "text/plain", "503 Server busy\n");
            closesocket(csock);
        }
    }
}
