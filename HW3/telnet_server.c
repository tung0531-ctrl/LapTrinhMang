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
#define popen _popen
#define pclose _pclose
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

#define READ_BUFFER_SIZE 2048
#define WRITE_BUFFER_SIZE 16384
#define FIELD_SIZE 128

struct client_context {
    socket_t socket_fd;
    struct sockaddr_in address;
    const char *database_file;
    char username[FIELD_SIZE];
    char password[FIELD_SIZE];
};

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

static int validate_account(const char *database_file, const char *username, const char *password) {
    FILE *file = fopen(database_file, "r");
    if (file == NULL) {
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), file) != NULL) {
        char file_user[FIELD_SIZE];
        char file_pass[FIELD_SIZE];

        if (sscanf(line, "%127s %127s", file_user, file_pass) == 2) {
            if (strcmp(file_user, username) == 0 && strcmp(file_pass, password) == 0) {
                fclose(file);
                return 1;
            }
        }
    }

    fclose(file);
    return 0;
}

static void append_to_output(char *buffer, size_t buffer_size, const char *text) {
    size_t current_length = strlen(buffer);
    if (current_length + 1 >= buffer_size) {
        return;
    }

    strncat(buffer, text, buffer_size - current_length - 1);
}

static int execute_command_to_buffer(const char *command, char *output, size_t output_size) {
    char out_file[128];
    char shell_command[1024];
    FILE *file;

    output[0] = '\0';
#ifdef _WIN32
    snprintf(out_file, sizeof(out_file), "telnet_out_%lu.txt", (unsigned long)GetCurrentProcessId());
    snprintf(shell_command,
             sizeof(shell_command),
             "cmd /c \"%s > \"\"%s\"\" 2>&1\"",
             command,
             out_file);
#else
    snprintf(out_file, sizeof(out_file), "telnet_out_%d.txt", getpid());
    snprintf(shell_command,
             sizeof(shell_command),
             "sh -c '%s > \"%s\" 2>&1'",
             command,
             out_file);
#endif

    if (system(shell_command) != 0) {
        append_to_output(output, output_size, "Lenh tra ve ma loi khac 0.\n");
    }

    file = fopen(out_file, "r");
    if (file == NULL) {
        append_to_output(output, output_size, "Khong mo duoc file ket qua lenh.\n");
        return -1;
    }

    while (fgets(shell_command, sizeof(shell_command), file) != NULL) {
        append_to_output(output, output_size, shell_command);
        if (strlen(output) + 64 >= output_size) {
            append_to_output(output, output_size, "\n[Da cat bot ket qua vi qua dai]\n");
            break;
        }
    }

    fclose(file);
    remove(out_file);
    return 0;
}

static int handle_telnet_session(struct client_context *context) {
    char line[READ_BUFFER_SIZE];
    char output[WRITE_BUFFER_SIZE];
    int logged_in = 0;

    if (send_all(context->socket_fd, "Username: ", strlen("Username: ")) < 0) {
        return -1;
    }

    while (!logged_in) {
        int status = receive_line(context->socket_fd, line, sizeof(line));
        if (status <= 0) {
            return -1;
        }

        strncpy(context->username, line, sizeof(context->username) - 1);
        context->username[sizeof(context->username) - 1] = '\0';

        if (send_all(context->socket_fd, "Password: ", strlen("Password: ")) < 0) {
            return -1;
        }

        status = receive_line(context->socket_fd, line, sizeof(line));
        if (status <= 0) {
            return -1;
        }

        strncpy(context->password, line, sizeof(context->password) - 1);
        context->password[sizeof(context->password) - 1] = '\0';

        if (validate_account(context->database_file, context->username, context->password)) {
            char client_ip[64] = "unknown";
            format_ipv4_address(&context->address, client_ip, sizeof(client_ip));
            printf("Dang nhap thanh cong %s:%d voi user %s\n",
                   client_ip,
                   ntohs(context->address.sin_port),
                   context->username);

            if (send_all(context->socket_fd,
                         "Dang nhap thanh cong.\nNhap lenh can thuc hien tren server.\n",
                         strlen("Dang nhap thanh cong.\nNhap lenh can thuc hien tren server.\n")) < 0) {
                return -1;
            }
            logged_in = 1;
        } else {
            if (send_all(context->socket_fd,
                         "Dang nhap that bai.\nUsername: ",
                         strlen("Dang nhap that bai.\nUsername: ")) < 0) {
                return -1;
            }
        }
    }

    while (1) {
        if (send_all(context->socket_fd, "telnet> ", strlen("telnet> ")) < 0) {
            return -1;
        }

        int status = receive_line(context->socket_fd, line, sizeof(line));
        if (status <= 0) {
            return -1;
        }

        if (line[0] == '\0') {
            continue;
        }

        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            send_all(context->socket_fd, "Tam biet.\n", strlen("Tam biet.\n"));
            return 0;
        }

        execute_command_to_buffer(line, output, sizeof(output));
        if (output[0] == '\0') {
            snprintf(output, sizeof(output), "[Lenh khong co du lieu tra ve]\n");
        }

        if (send_all(context->socket_fd, output, strlen(output)) < 0) {
            return -1;
        }
        if (output[strlen(output) - 1] != '\n') {
            if (send_all(context->socket_fd, "\n", 1) < 0) {
                return -1;
            }
        }
    }
}

#ifdef _WIN32
struct worker_arg {
    socket_t client_fd;
    struct sockaddr_in client_addr;
    const char *database_file;
};

static unsigned __stdcall client_worker(void *argument) {
    struct worker_arg *arg = (struct worker_arg *)argument;
    struct client_context context;
    char client_ip[64] = "unknown";

    memset(&context, 0, sizeof(context));
    context.socket_fd = arg->client_fd;
    context.address = arg->client_addr;
    context.database_file = arg->database_file;
    format_ipv4_address(&arg->client_addr, client_ip, sizeof(client_ip));
    printf("Client moi %s:%d\n", client_ip, ntohs(arg->client_addr.sin_port));

    handle_telnet_session(&context);
    close_socket(arg->client_fd);
    printf("Dong ket noi %s:%d\n", client_ip, ntohs(arg->client_addr.sin_port));
    free(arg);
    return 0;
}
#endif

int main(int argc, char *argv[]) {
    socket_t listener;
    struct sockaddr_in server_addr;
    const char *database_file = "accounts.txt";

    if (argc != 3 && argc != 4) {
        fprintf(stderr, "Usage: %s <ip> <port> [accounts_file]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (argc == 4) {
        database_file = argv[3];
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

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((unsigned short)atoi(argv[2]));

    if (!parse_ipv4_address(argv[1], &server_addr.sin_addr)) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", argv[1]);
        close_socket(listener);
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

    if (bind(listener, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind() failed");
        close_socket(listener);
        cleanup_socket_system();
        return EXIT_FAILURE;
    }

    if (listen(listener, 32) < 0) {
        perror("listen() failed");
        close_socket(listener);
        cleanup_socket_system();
        return EXIT_FAILURE;
    }

#ifndef _WIN32
    signal(SIGCHLD, reap_children);
#endif

    printf("Telnet multiprocessing server dang lang nghe %s:%s\n", argv[1], argv[2]);
    printf("File tai khoan su dung: %s\n", database_file);

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
            arg->database_file = database_file;

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
                struct client_context context;
                char client_ip[64] = "unknown";

                close_socket(listener);
                memset(&context, 0, sizeof(context));
                context.socket_fd = client_fd;
                context.address = client_addr;
                context.database_file = database_file;

                format_ipv4_address(&client_addr, client_ip, sizeof(client_ip));
                printf("Client moi %s:%d\n", client_ip, ntohs(client_addr.sin_port));

                handle_telnet_session(&context);
                close_socket(client_fd);
                printf("Dong ket noi %s:%d\n", client_ip, ntohs(client_addr.sin_port));
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
