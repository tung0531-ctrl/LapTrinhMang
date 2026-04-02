#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_CLIENTS FD_SETSIZE
#define READ_BUFFER_SIZE 1024
#define WRITE_BUFFER_SIZE 1024

enum client_state {
    STATE_SEND_NAME_PROMPT = 0,
    STATE_WAIT_NAME,
    STATE_SEND_MSSV_PROMPT,
    STATE_WAIT_MSSV,
    STATE_SEND_EMAIL
};

struct client_session {
    int active;
    int socket_fd;
    enum client_state state;
    struct sockaddr_in address;
    char read_buffer[READ_BUFFER_SIZE];
    size_t read_length;
    char write_buffer[WRITE_BUFFER_SIZE];
    size_t write_length;
    size_t write_sent;
    int close_after_send;
    char full_name[128];
    char mssv[32];
};

/* Đưa socket sang chế độ non-blocking để có thể phục vụ nhiều client đồng thời. */
static int set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }

    return 0;
}

/* Loại bỏ ký tự xuống dòng ở cuối dữ liệu người dùng gửi lên. */
static void trim_line(char *text) {
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
        text[length - 1] = '\0';
        length--;
    }
}

/* Chuẩn hóa một từ không dấu về dạng chỉ gồm chữ và số để ghép email. */
static void extract_name_token(const char *source, char *output, size_t output_size, int uppercase_first) {
    size_t output_index = 0;

    if (output_size == 0) {
        return;
    }

    for (size_t i = 0; source[i] != '\0' && output_index + 1 < output_size; ++i) {
        unsigned char current = (unsigned char)source[i];

        if (!isalnum(current)) {
            continue;
        }

        if (uppercase_first && output_index == 0) {
            output[output_index++] = (char)toupper(current);
        } else {
            output[output_index++] = (char)tolower(current);
        }
    }

    output[output_index] = '\0';
}

/* Tạo email theo quy tắc: TenCuoi.ChuCaiDau + 6 số cuối MSSV@sis.hust.edu.vn. */
static void build_student_email(const char *full_name, const char *mssv, char *output, size_t output_size) {
    char name_copy[128];
    char *tokens[16];
    int token_count = 0;
    char given_name[64];
    char initials[32];
    char mssv_suffix[32];

    strncpy(name_copy, full_name, sizeof(name_copy) - 1);
    name_copy[sizeof(name_copy) - 1] = '\0';

    char *token = strtok(name_copy, " \t");
    while (token != NULL && token_count < (int)(sizeof(tokens) / sizeof(tokens[0]))) {
        tokens[token_count++] = token;
        token = strtok(NULL, " \t");
    }

    if (strlen(mssv) > 2) {
        strncpy(mssv_suffix, mssv + 2, sizeof(mssv_suffix) - 1);
        mssv_suffix[sizeof(mssv_suffix) - 1] = '\0';
    } else {
        strncpy(mssv_suffix, mssv, sizeof(mssv_suffix) - 1);
        mssv_suffix[sizeof(mssv_suffix) - 1] = '\0';
    }

    if (token_count == 0) {
        snprintf(output, output_size, "Sinhvien.%s@sis.hust.edu.vn", mssv_suffix);
        return;
    }

    extract_name_token(tokens[token_count - 1], given_name, sizeof(given_name), 1);
    if (given_name[0] == '\0') {
        strcpy(given_name, "Sinhvien");
    }

    initials[0] = '\0';
    for (int i = 0; i < token_count - 1 && strlen(initials) + 1 < sizeof(initials); ++i) {
        char folded_token[32];
        size_t initials_length = strlen(initials);

        extract_name_token(tokens[i], folded_token, sizeof(folded_token), 0);
        if (folded_token[0] == '\0') {
            continue;
        }

        initials[initials_length] = (char)toupper((unsigned char)folded_token[0]);
        initials[initials_length + 1] = '\0';
    }

    if (initials[0] == '\0') {
        snprintf(output, output_size, "%s.%s@sis.hust.edu.vn", given_name, mssv_suffix);
        return;
    }

    snprintf(output, output_size, "%s.%s%s@sis.hust.edu.vn", given_name, initials, mssv_suffix);
}

/* Thu hồi tài nguyên của một phiên client trước khi tái sử dụng slot. */
static void reset_session(struct client_session *session) {
    if (session->active) {
        close(session->socket_fd);
    }

    memset(session, 0, sizeof(*session));
    session->socket_fd = -1;
}

/* Nạp thông điệp phản hồi vào bộ đệm gửi của client hiện tại. */
static void queue_message(struct client_session *session, const char *message, int close_after_send) {
    session->write_length = strlen(message);
    if (session->write_length >= sizeof(session->write_buffer)) {
        session->write_length = sizeof(session->write_buffer) - 1;
    }

    memcpy(session->write_buffer, message, session->write_length);
    session->write_buffer[session->write_length] = '\0';
    session->write_sent = 0;
    session->close_after_send = close_after_send;
}

/* Tìm một vị trí trống trong mảng session để gán cho client mới. */
static int find_free_slot(struct client_session sessions[], int max_sessions) {
    for (int i = 0; i < max_sessions; ++i) {
        if (!sessions[i].active) {
            return i;
        }
    }

    return -1;
}

/* Đóng kết nối và ghi log ngắn gọn ra màn hình. */
static void close_session(struct client_session *session) {
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &session->address.sin_addr, client_ip, sizeof(client_ip));
    printf("Dong ket noi %s:%d\n", client_ip, ntohs(session->address.sin_port));
    reset_session(session);
}

/* Xử lý một dòng hoàn chỉnh mà client vừa gửi, tùy theo trạng thái hiện tại. */
static void handle_complete_line(struct client_session *session, char *line) {
    trim_line(line);

    if (session->state == STATE_WAIT_NAME) {
        strncpy(session->full_name, line, sizeof(session->full_name) - 1);
        session->full_name[sizeof(session->full_name) - 1] = '\0';
        queue_message(session, "MSSV: ", 0);
        session->state = STATE_SEND_MSSV_PROMPT;
        return;
    }

    if (session->state == STATE_WAIT_MSSV) {
        char email[256];
        strncpy(session->mssv, line, sizeof(session->mssv) - 1);
        session->mssv[sizeof(session->mssv) - 1] = '\0';
        build_student_email(session->full_name, session->mssv, email, sizeof(email));
        snprintf(session->write_buffer,
                 sizeof(session->write_buffer),
                 "Email sinh vien: %s\n",
                 email);
        session->write_length = strlen(session->write_buffer);
        session->write_sent = 0;
        session->close_after_send = 1;
        session->state = STATE_SEND_EMAIL;

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &session->address.sin_addr, client_ip, sizeof(client_ip));
        printf("Da tao email cho %s:%d | Ho ten: %s | MSSV: %s | Email: %s\n",
               client_ip,
               ntohs(session->address.sin_port),
               session->full_name,
               session->mssv,
               email);
    }
}

/* Đọc dữ liệu không chặn từ client và ghép cho tới khi đủ một dòng hoàn chỉnh. */
static int process_read(struct client_session *session) {
    while (1) {
        ssize_t received = recv(session->socket_fd,
                                session->read_buffer + session->read_length,
                                sizeof(session->read_buffer) - session->read_length - 1,
                                0);

        if (received > 0) {
            session->read_length += (size_t)received;
            session->read_buffer[session->read_length] = '\0';

            char *newline = strchr(session->read_buffer, '\n');
            if (newline != NULL) {
                size_t line_length = (size_t)(newline - session->read_buffer + 1);
                char line[READ_BUFFER_SIZE];
                memcpy(line, session->read_buffer, line_length);
                line[line_length] = '\0';

                memmove(session->read_buffer,
                        session->read_buffer + line_length,
                        session->read_length - line_length + 1);
                session->read_length -= line_length;
                handle_complete_line(session, line);
                return 0;
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

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }

        perror("recv() failed");
        return -1;
    }
}

/* Gửi dần nội dung trong bộ đệm ra socket, xử lý cả trường hợp gửi chưa hết. */
static int process_write(struct client_session *session) {
    while (session->write_sent < session->write_length) {
        ssize_t sent = send(session->socket_fd,
                            session->write_buffer + session->write_sent,
                            session->write_length - session->write_sent,
                            0);

        if (sent > 0) {
            session->write_sent += (size_t)sent;
            continue;
        }

        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }

        perror("send() failed");
        return -1;
    }

    session->write_length = 0;
    session->write_sent = 0;

    if (session->state == STATE_SEND_NAME_PROMPT) {
        session->state = STATE_WAIT_NAME;
        return 0;
    }

    if (session->state == STATE_SEND_MSSV_PROMPT) {
        session->state = STATE_WAIT_MSSV;
        return 0;
    }

    if (session->close_after_send) {
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    /* Chương trình chỉ nhận một tham số là cổng lắng nghe của server. */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    /* Tạo listener TCP, cho phép tái sử dụng cổng và chuyển sang non-blocking. */
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) {
        perror("socket() failed");
        return EXIT_FAILURE;
    }

    int opt = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt() failed");
        close(listener);
        return EXIT_FAILURE;
    }

    if (set_non_blocking(listener) < 0) {
        perror("fcntl() failed");
        close(listener);
        return EXIT_FAILURE;
    }

    /* Gắn địa chỉ IP bất kỳ của máy hiện tại với cổng do người dùng cung cấp. */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((unsigned short)port);

    if (bind(listener, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind() failed");
        close(listener);
        return EXIT_FAILURE;
    }

    if (listen(listener, 16) < 0) {
        perror("listen() failed");
        close(listener);
        return EXIT_FAILURE;
    }

    /* Khởi tạo toàn bộ danh sách session cho nhiều client đồng thời. */
    struct client_session sessions[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        memset(&sessions[i], 0, sizeof(sessions[i]));
        sessions[i].socket_fd = -1;
    }

    printf("Email server non-blocking dang lang nghe cong %d\n", port);
   

    while (1) {
        /* Mỗi vòng lặp xây lại tập socket cần đọc và ghi cho select(). */
        fd_set read_set;
        fd_set write_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);

        FD_SET(listener, &read_set);
        int max_fd = listener;

        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (!sessions[i].active) {
                continue;
            }

            if (sessions[i].state == STATE_WAIT_NAME || sessions[i].state == STATE_WAIT_MSSV) {
                FD_SET(sessions[i].socket_fd, &read_set);
            }

            if (sessions[i].write_length > sessions[i].write_sent) {
                FD_SET(sessions[i].socket_fd, &write_set);
            }

            if (sessions[i].socket_fd > max_fd) {
                max_fd = sessions[i].socket_fd;
            }
        }

        if (select(max_fd + 1, &read_set, &write_set, NULL, NULL) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select() failed");
            break;
        }

        /* Nhận hết các kết nối mới đang chờ và đưa từng client vào mảng session. */
        if (FD_ISSET(listener, &read_set)) {
            while (1) {
                struct sockaddr_in client_addr;
                socklen_t client_length = sizeof(client_addr);
                int client_fd = accept(listener, (struct sockaddr *)&client_addr, &client_length);
                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    perror("accept() failed");
                    break;
                }

                if (set_non_blocking(client_fd) < 0) {
                    perror("fcntl() failed");
                    close(client_fd);
                    continue;
                }

                int slot = find_free_slot(sessions, MAX_CLIENTS);
                if (slot < 0) {
                    fprintf(stderr, "Khong con slot client trong server\n");
                    close(client_fd);
                    continue;
                }

                sessions[slot].active = 1;
                sessions[slot].socket_fd = client_fd;
                sessions[slot].state = STATE_SEND_NAME_PROMPT;
                sessions[slot].address = client_addr;
                queue_message(&sessions[slot], "Ho ten: ", 0);

                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
                printf("Client moi %s:%d\n", client_ip, ntohs(client_addr.sin_port));
            }
        }

        /* Duyệt các client đang hoạt động để xử lý đọc/ghi tương ứng. */
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (!sessions[i].active) {
                continue;
            }

            int should_close = 0;

            if (FD_ISSET(sessions[i].socket_fd, &write_set)) {
                if (process_write(&sessions[i]) < 0) {
                    should_close = 1;
                }
            }

            if (!should_close && FD_ISSET(sessions[i].socket_fd, &read_set)) {
                if (process_read(&sessions[i]) < 0) {
                    should_close = 1;
                }
            }

            if (should_close) {
                close_session(&sessions[i]);
            }
        }
    }

    close(listener);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (sessions[i].active) {
            reset_session(&sessions[i]);
        }
    }

    return EXIT_SUCCESS;
}