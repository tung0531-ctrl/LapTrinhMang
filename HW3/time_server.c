#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

#define BUFFER_SIZE 2048

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

static int format_ipv4_address(const struct sockaddr_in *address, char *buffer, size_t buffer_size) {
#ifdef _WIN32
    const char *text = inet_ntoa(address->sin_addr);
    if (text == NULL) {
        return -1;
    }

    snprintf(buffer, buffer_size, "%s", text);
    return 0;
#else
    return inet_ntop(AF_INET, &address->sin_addr, buffer, (socklen_t)buffer_size) == NULL ? -1 : 0;
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

static int receive_line(socket_t sockfd, char *buffer, size_t buffer_size) {
    size_t used = 0;

    while (used + 1 < buffer_size) {
        char ch;
        io_result_t received = recv(sockfd, &ch, 1, 0);
        if (received < 0) {
#ifndef _WIN32
            if (errno == EINTR) {
                continue;
            }
#endif
            return -1;
        }
        if (received == 0) {
            return 0;
        }

        buffer[used++] = ch;
        if (ch == '\n') {
            break;
        }
    }

    buffer[used] = '\0';
    trim_line(buffer);
    return 1;
}

static int build_time_string(const char *format_key, char *output, size_t output_size) {
    time_t now = time(NULL);
    struct tm local_tm;
    const char *time_pattern = NULL;

#ifdef _WIN32
    {
        struct tm *tmp = localtime(&now);
        if (tmp == NULL) {
            return -1;
        }
        local_tm = *tmp;
    }
#else
    if (localtime_r(&now, &local_tm) == NULL) {
        return -1;
    }
#endif

    if (strcmp(format_key, "dd/mm/yyyy") == 0) {
        time_pattern = "%d/%m/%Y";
    } else if (strcmp(format_key, "dd/mm/yy") == 0) {
        time_pattern = "%d/%m/%y";
    } else if (strcmp(format_key, "mm/dd/yyyy") == 0) {
        time_pattern = "%m/%d/%Y";
    } else if (strcmp(format_key, "mm/dd/yy") == 0) {
        time_pattern = "%m/%d/%y";
    } else {
        return -1;
    }

    if (strftime(output, output_size, time_pattern, &local_tm) == 0) {
        return -1;
    }

    return 0;
}

static int parse_get_time_command(const char *line, char *format_key, size_t format_size) {
    char command[64];
    char tail[64];
    int count = sscanf(line, "%63s %63s %63s", command, format_key, tail);

    if (count == 1 && strcmp(command, "GET_TIME") == 0) {
        snprintf(format_key, format_size, "dd/mm/yyyy");
        return 0;
    }

    if (count == 2 && strcmp(command, "GET_TIME") == 0) {
        return 0;
    }

    return -1;
}

static int handle_time_client(socket_t client_fd, const struct sockaddr_in *client_addr) {
    char client_ip[64] = "unknown";
    char line[BUFFER_SIZE];
    char format_key[64];
    char time_text[64];
    char response[256];

    format_ipv4_address(client_addr, client_ip, sizeof(client_ip));
    printf("Client moi %s:%d\n", client_ip, ntohs(client_addr->sin_port));

    if (send_all(client_fd,
                 "Time server san sang. Dung lenh: GET_TIME [format]\n"
                 "Format ho tro: dd/mm/yyyy, dd/mm/yy, mm/dd/yyyy, mm/dd/yy\n",
                 strlen("Time server san sang. Dung lenh: GET_TIME [format]\n"
                        "Format ho tro: dd/mm/yyyy, dd/mm/yy, mm/dd/yyyy, mm/dd/yy\n")) < 0) {
        return -1;
    }

    while (1) {
        int status = receive_line(client_fd, line, sizeof(line));
        if (status <= 0) {
            return -1;
        }

        if (line[0] == '\0') {
            if (send_all(client_fd, "ERR Empty command\n", strlen("ERR Empty command\n")) < 0) {
                return -1;
            }
            continue;
        }

        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            send_all(client_fd, "Bye\n", strlen("Bye\n"));
            return 0;
        }

        if (parse_get_time_command(line, format_key, sizeof(format_key)) < 0) {
            if (send_all(client_fd,
                         "ERR Invalid command. Use: GET_TIME [format]\n",
                         strlen("ERR Invalid command. Use: GET_TIME [format]\n")) < 0) {
                return -1;
            }
            continue;
        }

        if (build_time_string(format_key, time_text, sizeof(time_text)) < 0) {
            if (send_all(client_fd,
                         "ERR Unsupported format. Use: dd/mm/yyyy | dd/mm/yy | mm/dd/yyyy | mm/dd/yy\n",
                         strlen("ERR Unsupported format. Use: dd/mm/yyyy | dd/mm/yy | mm/dd/yyyy | mm/dd/yy\n")) < 0) {
                return -1;
            }
            continue;
        }

        snprintf(response, sizeof(response), "OK %s\n", time_text);
        if (send_all(client_fd, response, strlen(response)) < 0) {
            return -1;
        }
    }
}

#ifdef _WIN32
struct worker_arg {
    socket_t client_fd;
    struct sockaddr_in client_addr;
};

static unsigned __stdcall client_worker(void *argument) {
    struct worker_arg *arg = (struct worker_arg *)argument;
    handle_time_client(arg->client_fd, &arg->client_addr);
    close_socket(arg->client_fd);
    free(arg);
    return 0;
}
#endif

int main(int argc, char *argv[]) {
    socket_t listener;
    struct sockaddr_in server_addr;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <ip> <port>\n", argv[0]);
        return EXIT_FAILURE;
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

    if (listen(listener, 64) < 0) {
        perror("listen() failed");
        close_socket(listener);
        cleanup_socket_system();
        return EXIT_FAILURE;
    }

#ifndef _WIN32
    signal(SIGCHLD, reap_children);
#endif

    printf("Time server dang lang nghe %s:%s\n", argv[1], argv[2]);
    printf("Lenh ho tro: GET_TIME [format]\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_length = (socklen_t)sizeof(client_addr);
        socket_t client_fd = accept(listener, (struct sockaddr *)&client_addr, &client_length);

        if (client_fd == INVALID_SOCKET) {
            perror("accept() failed");
            continue;
        }

#ifdef _WIN32
        {
            struct worker_arg *arg = (struct worker_arg *)malloc(sizeof(struct worker_arg));
            uintptr_t thread_id;
            HANDLE worker;
            if (arg == NULL) {
                fprintf(stderr, "Khong cap phat bo nho cho worker\n");
                close_socket(client_fd);
                continue;
            }

            arg->client_fd = client_fd;
            arg->client_addr = client_addr;

            worker = (HANDLE)_beginthreadex(NULL, 0, client_worker, arg, 0, (unsigned *)&thread_id);
            if (worker == NULL) {
                fprintf(stderr, "Khong tao duoc worker thread\n");
                close_socket(client_fd);
                free(arg);
                continue;
            }

            CloseHandle(worker);
        }
#else
        {
            pid_t child = fork();
            if (child < 0) {
                perror("fork() failed");
                close_socket(client_fd);
                continue;
            }

            if (child == 0) {
                close_socket(listener);
                handle_time_client(client_fd, &client_addr);
                close_socket(client_fd);
                return EXIT_SUCCESS;
            }

            close_socket(client_fd);
        }
#endif
    }

    close_socket(listener);
    cleanup_socket_system();
    return EXIT_SUCCESS;
}
