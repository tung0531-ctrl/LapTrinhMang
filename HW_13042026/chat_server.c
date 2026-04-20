#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
typedef int io_result_t;
typedef int addrlen_t;
typedef struct pollfd {
    SOCKET fd;
    SHORT events;
    SHORT revents;
} pollfd_t;
#define close_socket closesocket
#define POLLIN 0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int socket_t;
typedef ssize_t io_result_t;
typedef socklen_t addrlen_t;
typedef struct pollfd pollfd_t;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define close_socket close
#endif

#define MAX_CLIENTS FD_SETSIZE
#define READ_BUFFER_SIZE 2048
#define WRITE_BUFFER_SIZE 8192

struct client_session {
    int active;
    socket_t socket_fd;
    int registered;
    struct sockaddr_in address;
    char client_id[64];
    char client_name[128];
    char read_buffer[READ_BUFFER_SIZE];
    size_t read_length;
    char write_buffer[WRITE_BUFFER_SIZE];
    size_t write_length;
    size_t write_sent;
};

#ifdef _WIN32
static int poll(pollfd_t fds[], unsigned long nfds, int timeout) {
    fd_set read_set;
    fd_set write_set;
    fd_set error_set;
    struct timeval timeout_value;
    struct timeval *timeout_ptr;
    SOCKET max_fd = 0;
    unsigned long index;
    int ready_count;

    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_ZERO(&error_set);

    for (index = 0; index < nfds; ++index) {
        fds[index].revents = 0;
        if (fds[index].fd == INVALID_SOCKET) {
            continue;
        }

        if (fds[index].events & POLLIN) {
            FD_SET(fds[index].fd, &read_set);
        }
        if (fds[index].events & POLLOUT) {
            FD_SET(fds[index].fd, &write_set);
        }
        FD_SET(fds[index].fd, &error_set);

        if (fds[index].fd > max_fd) {
            max_fd = fds[index].fd;
        }
    }

    if (timeout < 0) {
        timeout_ptr = NULL;
    } else {
        timeout_value.tv_sec = timeout / 1000;
        timeout_value.tv_usec = (timeout % 1000) * 1000;
        timeout_ptr = &timeout_value;
    }

    ready_count = select((int)max_fd + 1, &read_set, &write_set, &error_set, timeout_ptr);
    if (ready_count <= 0) {
        return ready_count;
    }

    ready_count = 0;
    for (index = 0; index < nfds; ++index) {
        if (fds[index].fd == INVALID_SOCKET) {
            continue;
        }

        if (FD_ISSET(fds[index].fd, &read_set)) {
            fds[index].revents = (SHORT)(fds[index].revents | POLLIN);
        }
        if (FD_ISSET(fds[index].fd, &write_set)) {
            fds[index].revents = (SHORT)(fds[index].revents | POLLOUT);
        }
        if (FD_ISSET(fds[index].fd, &error_set)) {
            fds[index].revents = (SHORT)(fds[index].revents | POLLERR);
        }

        if (fds[index].revents != 0) {
            ready_count++;
        }
    }

    return ready_count;
}
#endif

static int set_non_blocking(int fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket((SOCKET)fd, FIONBIO, &mode) == 0 ? 0 : -1;
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

static void fill_local_time(struct tm *time_info, time_t current_time) {
#ifdef _WIN32
    struct tm *local_value = localtime(&current_time);
    if (local_value != NULL) {
        *time_info = *local_value;
    } else {
        memset(time_info, 0, sizeof(*time_info));
    }
#else
    localtime_r(&current_time, time_info);
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

static void trim_line(char *text) {
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
        text[length - 1] = '\0';
        length--;
    }
}

static int is_compact_token(const char *text) {
    size_t i;

    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    for (i = 0; text[i] != '\0'; ++i) {
        if (isspace((unsigned char)text[i])) {
            return 0;
        }
    }

    return 1;
}

static void reset_session(struct client_session *session) {
    if (session->active) {
        close_socket(session->socket_fd);
    }

    memset(session, 0, sizeof(*session));
    session->socket_fd = -1;
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

static int find_free_slot(struct client_session sessions[]) {
    int i;

    for (i = 0; i < MAX_CLIENTS; ++i) {
        if (!sessions[i].active) {
            return i;
        }
    }

    return -1;
}

static int client_id_exists(struct client_session sessions[], const char *client_id) {
    int i;

    for (i = 0; i < MAX_CLIENTS; ++i) {
        if (sessions[i].active && sessions[i].registered && strcmp(sessions[i].client_id, client_id) == 0) {
            return 1;
        }
    }

    return 0;
}

static void format_timestamp(char *buffer, size_t buffer_size) {
    time_t now = time(NULL);
    struct tm local_tm;

    fill_local_time(&local_tm, now);
    strftime(buffer, buffer_size, "%Y/%m/%d %I:%M:%S%p", &local_tm);
}

static void broadcast_message(struct client_session sessions[], int sender_fd, const char *message) {
    int i;

    for (i = 0; i < MAX_CLIENTS; ++i) {
        if (!sessions[i].active || !sessions[i].registered || sessions[i].socket_fd == (socket_t)sender_fd) {
            continue;
        }

        if (append_message(&sessions[i], message) < 0) {
            fprintf(stderr, "Bo qua tin nhan den %s vi bo dem gui da day\n", sessions[i].client_id);
        }
    }
}

static void close_session(struct client_session sessions[], struct client_session *session) {
    if (session->registered) {
        char notice[256];
        snprintf(notice,
                 sizeof(notice),
                 "[Server] %s (%s) da roi phong chat\n",
                 session->client_id,
                 session->client_name);
        broadcast_message(sessions, (int)session->socket_fd, notice);
    }

    {
        char client_ip[INET_ADDRSTRLEN] = "unknown";
        format_ipv4_address(&session->address, client_ip, sizeof(client_ip));
        printf("Dong ket noi %s:%d\n", client_ip, ntohs(session->address.sin_port));
    }
    reset_session(session);
}

static int register_client(struct client_session sessions[], struct client_session *session, char *line) {
    char *separator = strstr(line, ":");
    char *client_id;
    char *client_name;

    if (separator == NULL) {
        append_message(session,
                       "Sai cu phap. Hay gui theo dang client_id: client_name\n"
                       "Dang ky: ");
        return 0;
    }

    *separator = '\0';
    client_id = line;
    client_name = separator + 1;

    while (*client_name == ' ' || *client_name == '\t') {
        client_name++;
    }

    trim_line(client_id);
    trim_line(client_name);

    if (!is_compact_token(client_id) || !is_compact_token(client_name)) {
        append_message(session,
                       "client_id va client_name phai la xau viet lien, khong co khoang trang\n"
                       "Dang ky: ");
        return 0;
    }

    if (client_id_exists(sessions, client_id)) {
        append_message(session,
                       "client_id da ton tai, vui long chon client_id khac\n"
                       "Dang ky: ");
        return 0;
    }

    strncpy(session->client_id, client_id, sizeof(session->client_id) - 1);
    session->client_id[sizeof(session->client_id) - 1] = '\0';
    strncpy(session->client_name, client_name, sizeof(session->client_name) - 1);
    session->client_name[sizeof(session->client_name) - 1] = '\0';
    session->registered = 1;

    {
        char welcome[256];
        char joined[256];

        snprintf(welcome,
                 sizeof(welcome),
                 "Dang ky thanh cong. Xin chao %s (%s)\n",
                 session->client_id,
                 session->client_name);
        append_message(session, welcome);

        snprintf(joined,
                 sizeof(joined),
                 "[Server] %s (%s) da tham gia phong chat\n",
                 session->client_id,
                 session->client_name);
        broadcast_message(sessions, session->socket_fd, joined);
    }

    printf("Client dang ky thanh cong: %s (%s)\n", session->client_id, session->client_name);
    return 0;
}

static int handle_chat_line(struct client_session sessions[], struct client_session *session, char *line) {
    trim_line(line);

    if (line[0] == '\0') {
        return 0;
    }

    if (!session->registered) {
        return register_client(sessions, session, line);
    }

    {
        char timestamp[64];
        char message[READ_BUFFER_SIZE + 128];

        format_timestamp(timestamp, sizeof(timestamp));
        snprintf(message, sizeof(message), "%s %s: %s\n", timestamp, session->client_id, line);
        broadcast_message(sessions, session->socket_fd, message);
        printf("%s", message);
    }
    return 0;
}

static int process_read(struct client_session sessions[], struct client_session *session) {
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

                {
                    size_t line_length = (size_t)(newline - session->read_buffer + 1);
                    char line[READ_BUFFER_SIZE];

                    memcpy(line, session->read_buffer, line_length);
                    line[line_length] = '\0';

                    memmove(session->read_buffer,
                            session->read_buffer + line_length,
                            session->read_length - line_length + 1);
                    session->read_length -= line_length;

                    if (handle_chat_line(sessions, session, line) < 0) {
                        return -1;
                    }
                }
            }

            if (session->read_length == sizeof(session->read_buffer) - 1) {
                fprintf(stderr, "Dong du lieu qua dai, dong ket noi client\n");
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

    session->write_length = 0;
    session->write_sent = 0;
    session->write_buffer[0] = '\0';
    return 0;
}

int main(int argc, char *argv[]) {
    socket_t listener;
    struct sockaddr_in server_addr;
    struct client_session sessions[MAX_CLIENTS];
    pollfd_t pollfds[MAX_CLIENTS + 1];
    int i;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    i = atoi(argv[1]);
    if (i <= 0 || i > 65535) {
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

    if (set_non_blocking((int)listener) < 0) {
        perror("fcntl() failed");
        close_socket(listener);
        cleanup_socket_system();
        return EXIT_FAILURE;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((unsigned short)atoi(argv[1]));

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

    for (i = 0; i < MAX_CLIENTS; ++i) {
        memset(&sessions[i], 0, sizeof(sessions[i]));
        sessions[i].socket_fd = -1;
    }

    printf("Chat server dang lang nghe cong %s\n", argv[1]);
    printf("Client phai dang ky theo cu phap: client_id: client_name\n");

    while (1) {
        int poll_count = 1;
        int should_break = 0;

        pollfds[0].fd = listener;
        pollfds[0].events = POLLIN;
        pollfds[0].revents = 0;

        for (i = 0; i < MAX_CLIENTS; ++i) {
            if (!sessions[i].active) {
                continue;
            }

            pollfds[poll_count].fd = sessions[i].socket_fd;
            pollfds[poll_count].events = POLLIN;
            if (sessions[i].write_length > sessions[i].write_sent) {
                pollfds[poll_count].events = (short)(pollfds[poll_count].events | POLLOUT);
            }
            pollfds[poll_count].revents = 0;
            poll_count++;
        }

        if (poll(pollfds, poll_count, -1) < 0) {
#ifndef _WIN32
            if (errno == EINTR) {
                continue;
            }
#endif
            perror("poll() failed");
            break;
        }

        if (pollfds[0].revents & POLLIN) {
            while (1) {
                struct sockaddr_in client_addr;
                addrlen_t client_length = (addrlen_t)sizeof(client_addr);
                socket_t client_fd = accept(listener, (struct sockaddr *)&client_addr, &client_length);

                if (client_fd == INVALID_SOCKET) {
                    if (socket_would_block()) {
                        break;
                    }
                    perror("accept() failed");
                    should_break = 1;
                    break;
                }

                if (set_non_blocking((int)client_fd) < 0) {
                    perror("fcntl() failed");
                    close_socket(client_fd);
                    continue;
                }

                {
                    int slot = find_free_slot(sessions);
                    if (slot < 0) {
                        fprintf(stderr, "Khong con slot client\n");
                        close_socket(client_fd);
                        continue;
                    }

                    sessions[slot].active = 1;
                    sessions[slot].socket_fd = client_fd;
                    sessions[slot].address = client_addr;
                    if (append_message(&sessions[slot],
                                       "Chao mung den chat server\n"
                                       "Hay dang ky theo cu phap client_id: client_name\n"
                                       "Dang ky: ") < 0) {
                        reset_session(&sessions[slot]);
                        continue;
                    }
                }

                {
                    char client_ip[INET_ADDRSTRLEN] = "unknown";
                    format_ipv4_address(&client_addr, client_ip, sizeof(client_ip));
                    printf("Client moi %s:%d\n", client_ip, ntohs(client_addr.sin_port));
                }
            }
        }

        if (should_break) {
            break;
        }

        for (i = 0; i < MAX_CLIENTS; ++i) {
            short revents;
            int should_close;

            if (!sessions[i].active) {
                continue;
            }

            revents = pollfds[i + 1].revents;
            should_close = 0;

            if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
                should_close = 1;
            }

            if (!should_close && (revents & POLLOUT)) {
                if (process_write(&sessions[i]) < 0) {
                    should_close = 1;
                }
            }

            if (!should_close && (revents & POLLIN)) {
                if (process_read(sessions, &sessions[i]) < 0) {
                    should_close = 1;
                }
            }

            if (should_close) {
                close_session(sessions, &sessions[i]);
            }
        }
    }

    close_socket(listener);
    for (i = 0; i < MAX_CLIENTS; ++i) {
        if (sessions[i].active) {
            reset_session(&sessions[i]);
        }
    }

    cleanup_socket_system();
    return EXIT_SUCCESS;
}