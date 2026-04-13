#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
typedef int io_result_t;
typedef int addrlen_t;
#define close_socket closesocket
#define popen _popen
#define pclose _pclose
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int socket_t;
typedef ssize_t io_result_t;
typedef socklen_t addrlen_t;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define close_socket close
#endif

#define MAX_CLIENTS FD_SETSIZE
#define READ_BUFFER_SIZE 2048
#define WRITE_BUFFER_SIZE 16384
#define FIELD_SIZE 128

enum client_state {
    STATE_SEND_USER_PROMPT = 0,
    STATE_WAIT_USER,
    STATE_SEND_PASS_PROMPT,
    STATE_WAIT_PASS,
    STATE_SEND_SHELL_PROMPT,
    STATE_WAIT_COMMAND
};

struct client_session {
    int active;
    socket_t socket_fd;
    enum client_state state;
    struct sockaddr_in address;
    char username[FIELD_SIZE];
    char password[FIELD_SIZE];
    char read_buffer[READ_BUFFER_SIZE];
    size_t read_length;
    char write_buffer[WRITE_BUFFER_SIZE];
    size_t write_length;
    size_t write_sent;
};

static int set_non_blocking(socket_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }

    return 0;
#endif
}

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

static int socket_would_block(void) {
#ifdef _WIN32
    int error_code = WSAGetLastError();
    return error_code == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

static void trim_line(char *text) {
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
        text[length - 1] = '\0';
        length--;
    }
}

static void append_to_output(char *buffer, size_t buffer_size, const char *text) {
    size_t current_length = strlen(buffer);
    if (current_length + 1 >= buffer_size) {
        return;
    }

    strncat(buffer, text, buffer_size - current_length - 1);
}

static void reset_session(struct client_session *session) {
    if (session->active) {
        close_socket(session->socket_fd);
    }

    memset(session, 0, sizeof(*session));
    session->socket_fd = INVALID_SOCKET;
}

static int append_message(struct client_session *session, const char *message) {
    size_t message_length = strlen(message);
    if (message_length > sizeof(session->write_buffer) - session->write_length - 1) {
        return -1;
    }

    memcpy(session->write_buffer + session->write_length, message, message_length);
    session->write_length += message_length;
    session->write_buffer[session->write_length] = '\0';
    return 0;
}

static void clear_pending_output(struct client_session *session) {
    session->write_length = 0;
    session->write_sent = 0;
    session->write_buffer[0] = '\0';
}

static int find_free_slot(struct client_session sessions[]) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!sessions[i].active) {
            return i;
        }
    }

    return -1;
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

static void close_session(struct client_session *session) {
    char client_ip[64] = "unknown";
    format_ipv4_address(&session->address, client_ip, sizeof(client_ip));
    printf("Dong ket noi %s:%d\n", client_ip, ntohs(session->address.sin_port));
    reset_session(session);
}

static void queue_prompt_for_state(struct client_session *session) {
    clear_pending_output(session);

    if (session->state == STATE_SEND_USER_PROMPT) {
        append_message(session, "Username: ");
        session->state = STATE_WAIT_USER;
        return;
    }

    if (session->state == STATE_SEND_PASS_PROMPT) {
        append_message(session, "Password: ");
        session->state = STATE_WAIT_PASS;
        return;
    }

    if (session->state == STATE_SEND_SHELL_PROMPT) {
        append_message(session, "telnet> ");
        session->state = STATE_WAIT_COMMAND;
    }
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

static int execute_command_to_buffer(const char *command, char *output, size_t output_size) {
    char out_file[128];
    char shell_command[1024];
    FILE *file;
    int process_id;

    output[0] = '\0';
#ifdef _WIN32
    process_id = _getpid();
#else
    process_id = getpid();
#endif
    snprintf(out_file, sizeof(out_file), "telnet_out_%d.txt", process_id);

#ifdef _WIN32
    snprintf(shell_command,
             sizeof(shell_command),
             "cmd /c \"%s > \"\"%s\"\" 2>&1\"",
             command,
             out_file);
#else
    snprintf(shell_command,
             sizeof(shell_command),
             "sh -c '%s > "%s" 2>&1'",
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

static int handle_login(struct client_session *session, const char *database_file) {
    if (validate_account(database_file, session->username, session->password)) {
        char client_ip[64] = "unknown";
        format_ipv4_address(&session->address, client_ip, sizeof(client_ip));
        printf("Dang nhap thanh cong %s:%d voi user %s\n",
               client_ip,
               ntohs(session->address.sin_port),
               session->username);

        clear_pending_output(session);
        if (append_message(session, "Dang nhap thanh cong.\n") < 0 ||
            append_message(session, "Nhap lenh can thuc hien tren server.\n") < 0) {
            return -1;
        }
        session->state = STATE_SEND_SHELL_PROMPT;
        return 0;
    }

    clear_pending_output(session);
    if (append_message(session, "Dang nhap that bai.\n") < 0) {
        return -1;
    }
    session->username[0] = '\0';
    session->password[0] = '\0';
    session->state = STATE_SEND_USER_PROMPT;
    return 0;
}

static int handle_command(struct client_session *session, const char *line) {
    char output[WRITE_BUFFER_SIZE];
    char prompt_buffer[64];

    if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
        clear_pending_output(session);
        append_message(session, "Tam biet.\n");
        return -1;
    }

    execute_command_to_buffer(line, output, sizeof(output));
    if (output[0] == '\0') {
        snprintf(output, sizeof(output), "[Lenh khong co du lieu tra ve]\n");
    }

    clear_pending_output(session);
    if (append_message(session, output) < 0) {
        return -1;
    }

    snprintf(prompt_buffer, sizeof(prompt_buffer), "%stelnet> ", output[strlen(output) - 1] == '\n' ? "" : "\n");
    if (append_message(session, prompt_buffer) < 0) {
        return -1;
    }
    session->state = STATE_WAIT_COMMAND;
    return 0;
}

static int handle_complete_line(struct client_session *session, const char *database_file, char *line) {
    trim_line(line);

    if (session->state == STATE_WAIT_USER) {
        strncpy(session->username, line, sizeof(session->username) - 1);
        session->username[sizeof(session->username) - 1] = '\0';
        session->state = STATE_SEND_PASS_PROMPT;
        queue_prompt_for_state(session);
        return 0;
    }

    if (session->state == STATE_WAIT_PASS) {
        strncpy(session->password, line, sizeof(session->password) - 1);
        session->password[sizeof(session->password) - 1] = '\0';
        return handle_login(session, database_file);
    }

    if (session->state == STATE_WAIT_COMMAND) {
        if (line[0] == '\0') {
            clear_pending_output(session);
            append_message(session, "telnet> ");
            return 0;
        }
        return handle_command(session, line);
    }

    return 0;
}

static int process_read(struct client_session *session, const char *database_file) {
    while (1) {
        io_result_t received = recv(session->socket_fd,
                                    session->read_buffer + session->read_length,
                                    (int)(sizeof(session->read_buffer) - session->read_length - 1),
                                    0);

        if (received > 0) {
            session->read_length += (size_t)received;
            session->read_buffer[session->read_length] = '\0';

            while (1) {
                char *newline = strchr(session->read_buffer, '\n');
                if (newline == NULL) {
                    break;
                }

                size_t line_length = (size_t)(newline - session->read_buffer + 1);
                char line[READ_BUFFER_SIZE];
                memcpy(line, session->read_buffer, line_length);
                line[line_length] = '\0';

                memmove(session->read_buffer,
                        session->read_buffer + line_length,
                        session->read_length - line_length + 1);
                session->read_length -= line_length;

                if (handle_complete_line(session, database_file, line) < 0) {
                    return -1;
                }
            }

            if (session->read_length == sizeof(session->read_buffer) - 1) {
                fprintf(stderr, "Input qua dai, dong ket noi client\n");
                return -1;
            }

            continue;
        }

        if (received == 0) {
            return -1;
        }

        if (socket_would_block()) {
            return 0;
        }

        perror("recv() failed");
        return -1;
    }
}

static int process_write(struct client_session *session) {
    while (session->write_sent < session->write_length) {
        io_result_t sent = send(session->socket_fd,
                                session->write_buffer + session->write_sent,
                                (int)(session->write_length - session->write_sent),
                                0);

        if (sent > 0) {
            session->write_sent += (size_t)sent;
            continue;
        }

        if (sent < 0 && socket_would_block()) {
            return 0;
        }

        perror("send() failed");
        return -1;
    }

    clear_pending_output(session);
    if (session->state == STATE_SEND_SHELL_PROMPT) {
        queue_prompt_for_state(session);
    }
    return 0;
}

int main(int argc, char *argv[]) {
    socket_t listener;
    struct sockaddr_in server_addr;
    struct client_session sessions[MAX_CLIENTS];
    const char *database_file = "accounts.txt";
    int port;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
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
        int opt = 1;
        if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt)) < 0) {
            perror("setsockopt() failed");
            close_socket(listener);
            cleanup_socket_system();
            return EXIT_FAILURE;
        }
    }

    if (set_non_blocking(listener) < 0) {
        perror("set_non_blocking() failed");
        close_socket(listener);
        cleanup_socket_system();
        return EXIT_FAILURE;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((unsigned short)port);

    if (bind(listener, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind() failed");
        close_socket(listener);
        cleanup_socket_system();
        return EXIT_FAILURE;
    }

    if (listen(listener, 16) < 0) {
        perror("listen() failed");
        close_socket(listener);
        cleanup_socket_system();
        return EXIT_FAILURE;
    }

    for (port = 0; port < MAX_CLIENTS; ++port) {
        memset(&sessions[port], 0, sizeof(sessions[port]));
        sessions[port].socket_fd = INVALID_SOCKET;
    }

    printf("Telnet server dang lang nghe cong %d\n", atoi(argv[1]));
    printf("File tai khoan su dung: %s\n", database_file);

    while (1) {
        fd_set read_set;
        fd_set write_set;
        socket_t max_fd = listener;

        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_SET(listener, &read_set);

        for (port = 0; port < MAX_CLIENTS; ++port) {
            if (!sessions[port].active) {
                continue;
            }

            FD_SET(sessions[port].socket_fd, &read_set);
            if (sessions[port].write_length > sessions[port].write_sent) {
                FD_SET(sessions[port].socket_fd, &write_set);
            }

            if (sessions[port].socket_fd > max_fd) {
                max_fd = sessions[port].socket_fd;
            }
        }

        if (select((int)max_fd + 1, &read_set, &write_set, NULL, NULL) < 0) {
#ifndef _WIN32
            if (errno == EINTR) {
                continue;
            }
#endif
            perror("select() failed");
            break;
        }

        if (FD_ISSET(listener, &read_set)) {
            while (1) {
                struct sockaddr_in client_addr;
                addrlen_t client_length = (addrlen_t)sizeof(client_addr);
                socket_t client_fd = accept(listener, (struct sockaddr *)&client_addr, &client_length);
                int slot;
                char client_ip[64] = "unknown";

                if (client_fd == INVALID_SOCKET) {
                    if (socket_would_block()) {
                        break;
                    }
                    perror("accept() failed");
                    break;
                }

                if (set_non_blocking(client_fd) < 0) {
                    perror("set_non_blocking() failed");
                    close_socket(client_fd);
                    continue;
                }

                slot = find_free_slot(sessions);
                if (slot < 0) {
                    fprintf(stderr, "Khong con slot client\n");
                    close_socket(client_fd);
                    continue;
                }

                memset(&sessions[slot], 0, sizeof(sessions[slot]));
                sessions[slot].active = 1;
                sessions[slot].socket_fd = client_fd;
                sessions[slot].address = client_addr;
                sessions[slot].state = STATE_SEND_USER_PROMPT;
                queue_prompt_for_state(&sessions[slot]);

                format_ipv4_address(&client_addr, client_ip, sizeof(client_ip));
                printf("Client moi %s:%d\n", client_ip, ntohs(client_addr.sin_port));
            }
        }

        for (port = 0; port < MAX_CLIENTS; ++port) {
            int should_close = 0;

            if (!sessions[port].active) {
                continue;
            }

            if (FD_ISSET(sessions[port].socket_fd, &write_set)) {
                if (process_write(&sessions[port]) < 0) {
                    should_close = 1;
                }
            }

            if (!should_close && FD_ISSET(sessions[port].socket_fd, &read_set)) {
                if (process_read(&sessions[port], database_file) < 0) {
                    should_close = 1;
                }
            }

            if (should_close) {
                close_session(&sessions[port]);
            }
        }
    }

    close_socket(listener);
    for (port = 0; port < MAX_CLIENTS; ++port) {
        if (sessions[port].active) {
            reset_session(&sessions[port]);
        }
    }

    cleanup_socket_system();
    return EXIT_SUCCESS;
}