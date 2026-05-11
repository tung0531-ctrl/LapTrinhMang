#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET socket_t;
typedef int io_result_t;
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
typedef int socket_t;
typedef ssize_t io_result_t;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define close_socket close
#endif

#define BUFFER_SIZE 4096

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

#ifndef _WIN32
static void reap_children(int signal_value) {
    (void)signal_value;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}
#endif

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

static void trim_line(char *text) {
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
        text[length - 1] = '\0';
        length--;
    }
}

static const char *content_type_from_path(const char *path) {
    const char *ext = strrchr(path, '.');
    if (ext == NULL) {
        return "text/plain";
    }

    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) {
        return "text/html";
    }
    if (strcmp(ext, ".txt") == 0) {
        return "text/plain";
    }
    if (strcmp(ext, ".json") == 0) {
        return "application/json";
    }
    if (strcmp(ext, ".css") == 0) {
        return "text/css";
    }
    if (strcmp(ext, ".js") == 0) {
        return "application/javascript";
    }
    return "application/octet-stream";
}

static int send_simple_response(socket_t client_fd,
                                int status_code,
                                const char *reason,
                                const char *content_type,
                                const char *body) {
    char header[512];
    size_t body_length = strlen(body);
    int header_length = snprintf(header,
                                 sizeof(header),
                                 "HTTP/1.1 %d %s\r\n"
                                 "Connection: close\r\n"
                                 "Content-Type: %s\r\n"
                                 "Content-Length: %zu\r\n"
                                 "\r\n",
                                 status_code,
                                 reason,
                                 content_type,
                                 body_length);

    if (header_length < 0 || (size_t)header_length >= sizeof(header)) {
        return -1;
    }

    if (send_all(client_fd, header, (size_t)header_length) < 0) {
        return -1;
    }

    return send_all(client_fd, body, body_length);
}

static int send_file_response(socket_t client_fd, const char *path) {
    FILE *file = fopen(path, "rb");
    char header[512];
    char buffer[BUFFER_SIZE];
    long file_size;

    if (file == NULL) {
        return send_simple_response(client_fd,
                                    404,
                                    "Not Found",
                                    "text/plain",
                                    "404 Not Found\n");
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return send_simple_response(client_fd,
                                    500,
                                    "Internal Server Error",
                                    "text/plain",
                                    "500 Internal Server Error\n");
    }

    file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        return send_simple_response(client_fd,
                                    500,
                                    "Internal Server Error",
                                    "text/plain",
                                    "500 Internal Server Error\n");
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return send_simple_response(client_fd,
                                    500,
                                    "Internal Server Error",
                                    "text/plain",
                                    "500 Internal Server Error\n");
    }

    {
        int header_length = snprintf(header,
                                     sizeof(header),
                                     "HTTP/1.1 200 OK\r\n"
                                     "Connection: close\r\n"
                                     "Content-Type: %s\r\n"
                                     "Content-Length: %ld\r\n"
                                     "\r\n",
                                     content_type_from_path(path),
                                     file_size);

        if (header_length < 0 || (size_t)header_length >= sizeof(header)) {
            fclose(file);
            return -1;
        }

        if (send_all(client_fd, header, (size_t)header_length) < 0) {
            fclose(file);
            return -1;
        }
    }

    while (1) {
        size_t bytes_read = fread(buffer, 1, sizeof(buffer), file);
        if (bytes_read > 0) {
            if (send_all(client_fd, buffer, bytes_read) < 0) {
                fclose(file);
                return -1;
            }
        }

        if (bytes_read < sizeof(buffer)) {
            if (ferror(file)) {
                fclose(file);
                return -1;
            }
            break;
        }
    }

    fclose(file);
    return 0;
}

static int sanitize_request_path(const char *raw_path, char *safe_path, size_t safe_size) {
    size_t i;
    if (raw_path == NULL || raw_path[0] == '\0') {
        snprintf(safe_path, safe_size, "index.html");
        return 0;
    }

    if (strcmp(raw_path, "/") == 0) {
        snprintf(safe_path, safe_size, "index.html");
        return 0;
    }

    if (raw_path[0] != '/') {
        return -1;
    }

    if (strstr(raw_path, "..") != NULL) {
        return -1;
    }

    for (i = 1; raw_path[i] != '\0' && i < safe_size; ++i) {
        if (raw_path[i] == '?') {
            break;
        }
        if (!(isalnum((unsigned char)raw_path[i]) || raw_path[i] == '/' || raw_path[i] == '.' || raw_path[i] == '_' || raw_path[i] == '-')) {
            return -1;
        }
        safe_path[i - 1] = raw_path[i];
    }

    safe_path[i - 1] = '\0';
    if (safe_path[0] == '\0') {
        snprintf(safe_path, safe_size, "index.html");
    }
    return 0;
}

static int handle_http_request(socket_t client_fd, int worker_id) {
    char buffer[BUFFER_SIZE];
    char method[16];
    char path[256];
    char version[32];
    char safe_path[256];
    io_result_t received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    if (received <= 0) {
        return -1;
    }

    buffer[received] = '\0';
    trim_line(buffer);

    if (sscanf(buffer, "%15s %255s %31s", method, path, version) != 3) {
        return send_simple_response(client_fd,
                                    400,
                                    "Bad Request",
                                    "text/plain",
                                    "400 Bad Request\n");
    }

    if (strcmp(method, "GET") != 0) {
        return send_simple_response(client_fd,
                                    405,
                                    "Method Not Allowed",
                                    "text/plain",
                                    "405 Method Not Allowed\n");
    }

    if (strcmp(version, "HTTP/1.0") != 0 && strcmp(version, "HTTP/1.1") != 0) {
        return send_simple_response(client_fd,
                                    505,
                                    "HTTP Version Not Supported",
                                    "text/plain",
                                    "505 HTTP Version Not Supported\n");
    }

    if (sanitize_request_path(path, safe_path, sizeof(safe_path)) < 0) {
        return send_simple_response(client_fd,
                                    400,
                                    "Bad Request",
                                    "text/plain",
                                    "400 Bad Request\n");
    }

    printf("Worker %d xu ly: %s %s\n", worker_id, method, path);
    return send_file_response(client_fd, safe_path);
}

#ifdef _WIN32
struct worker_arg {
    socket_t listener;
    int worker_id;
};

static unsigned __stdcall worker_loop(void *argument) {
    struct worker_arg *arg = (struct worker_arg *)argument;

    while (1) {
        struct sockaddr_in client_addr;
        int client_length = (int)sizeof(client_addr);
        socket_t client_fd = accept(arg->listener, (struct sockaddr *)&client_addr, &client_length);

        if (client_fd == INVALID_SOCKET) {
            continue;
        }

        handle_http_request(client_fd, arg->worker_id);
        close_socket(client_fd);
    }

    return 0;
}
#else
static void worker_loop(socket_t listener, int worker_id) {
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_length = (socklen_t)sizeof(client_addr);
        socket_t client_fd = accept(listener, (struct sockaddr *)&client_addr, &client_length);

        if (client_fd == INVALID_SOCKET) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept() failed");
            continue;
        }

        handle_http_request(client_fd, worker_id);
        close_socket(client_fd);
    }
}
#endif

int main(int argc, char *argv[]) {
    socket_t listener;
    struct sockaddr_in server_addr;
    int worker_count = 4;

    if (argc != 3 && argc != 4) {
        fprintf(stderr, "Usage: %s <ip> <port> [worker_count]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (argc == 4) {
        worker_count = atoi(argv[3]);
        if (worker_count <= 0 || worker_count > 64) {
            fprintf(stderr, "worker_count phai trong khoang 1..64\n");
            return EXIT_FAILURE;
        }
    }

    if (initialize_socket_system() < 0) {
        fprintf(stderr, "Khong khoi tao duoc socket system\n");
        return EXIT_FAILURE;
    }

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        perror("socket() failed");
        cleanup_socket_system();
        return EXIT_FAILURE;
    }

    {
        int reuse_value = 1;
        if (setsockopt(listener,
                       SOL_SOCKET,
                       SO_REUSEADDR,
                       (const char *)&reuse_value,
                       (int)sizeof(reuse_value)) < 0) {
            perror("setsockopt() failed");
            close_socket(listener);
            cleanup_socket_system();
            return EXIT_FAILURE;
        }
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((unsigned short)atoi(argv[2]));

    if (!parse_ipv4_address(argv[1], &server_addr.sin_addr)) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", argv[1]);
        close_socket(listener);
        cleanup_socket_system();
        return EXIT_FAILURE;
    }

    if (bind(listener, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind() failed");
        close_socket(listener);
        cleanup_socket_system();
        return EXIT_FAILURE;
    }

    if (listen(listener, 128) < 0) {
        perror("listen() failed");
        close_socket(listener);
        cleanup_socket_system();
        return EXIT_FAILURE;
    }

    printf("HTTP server dang lang nghe %s:%s\n", argv[1], argv[2]);
    printf("So worker prefork: %d\n", worker_count);

#ifdef _WIN32
    {
        struct worker_arg *args = (struct worker_arg *)calloc((size_t)worker_count, sizeof(struct worker_arg));
        HANDLE *handles = (HANDLE *)calloc((size_t)worker_count, sizeof(HANDLE));
        int i;

        if (args == NULL || handles == NULL) {
            fprintf(stderr, "Khong cap phat bo nho cho worker\n");
            free(args);
            free(handles);
            close_socket(listener);
            cleanup_socket_system();
            return EXIT_FAILURE;
        }

        for (i = 0; i < worker_count; ++i) {
            uintptr_t thread_id;
            args[i].listener = listener;
            args[i].worker_id = i + 1;
            handles[i] = (HANDLE)_beginthreadex(NULL, 0, worker_loop, &args[i], 0, (unsigned *)&thread_id);
            if (handles[i] == NULL) {
                fprintf(stderr, "Khong tao duoc worker %d\n", i + 1);
                continue;
            }
        }

        WaitForMultipleObjects((DWORD)worker_count, handles, TRUE, INFINITE);
        for (i = 0; i < worker_count; ++i) {
            if (handles[i] != NULL) {
                CloseHandle(handles[i]);
            }
        }
        free(args);
        free(handles);
    }
#else
    {
        int i;
        signal(SIGCHLD, reap_children);

        for (i = 0; i < worker_count; ++i) {
            pid_t child = fork();
            if (child < 0) {
                perror("fork() failed");
                close_socket(listener);
                cleanup_socket_system();
                return EXIT_FAILURE;
            }

            if (child == 0) {
                worker_loop(listener, i + 1);
                close_socket(listener);
                cleanup_socket_system();
                return EXIT_SUCCESS;
            }
        }

        while (1) {
            pause();
        }
    }
#endif

    close_socket(listener);
    cleanup_socket_system();
    return EXIT_SUCCESS;
}
