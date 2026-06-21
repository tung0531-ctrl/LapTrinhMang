#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "Ws2_32.lib")
typedef SOCKET socket_t;
typedef int io_result_t;
#define close_socket closesocket
#define shutdown_both(sock) shutdown((sock), SD_BOTH)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int socket_t;
typedef ssize_t io_result_t;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define close_socket close
#define shutdown_both(sock) shutdown((sock), SHUT_RDWR)
#endif

#define FTP_HOST "lebavui.io.vn"
#define FTP_PORT "21"
#define FTP_USERNAME "user_20235454"
#define FTP_PASSWORD "545431"

#define BUFFER_SIZE 4096
#define RESPONSE_SIZE 2048
#define PATH_SIZE 512

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

static int send_all(socket_t sockfd, const void *buffer, size_t length) {
    size_t sent_total = 0;
    const char *data = (const char *)buffer;

    while (sent_total < length) {
        io_result_t sent = send(sockfd, data + sent_total, (int)(length - sent_total), 0);
        if (sent == SOCKET_ERROR) {
            return -1;
        }
        sent_total += (size_t)sent;
    }

    return 0;
}

static int recv_line(socket_t sockfd, char *buffer, size_t buffer_size) {
    size_t used = 0;

    if (buffer_size == 0) {
        return -1;
    }

    while (used + 1 < buffer_size) {
        char ch;
        io_result_t received = recv(sockfd, &ch, 1, 0);
        if (received == SOCKET_ERROR) {
            return -1;
        }
        if (received == 0) {
            if (used == 0) {
                return 0;
            }
            break;
        }

        buffer[used++] = ch;
        if (ch == '\n') {
            break;
        }
    }

    buffer[used] = '\0';
    return (int)used;
}

static void trim_crlf(char *text) {
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\r' || text[length - 1] == '\n')) {
        text[length - 1] = '\0';
        length--;
    }
}

static int read_ftp_response(socket_t control_socket, int *response_code, char *response_text, size_t response_text_size) {
    char line[RESPONSE_SIZE];
    int expected_code = -1;
    int multiline = 0;

    if (response_text_size > 0) {
        response_text[0] = '\0';
    }

    while (1) {
        int received = recv_line(control_socket, line, sizeof(line));
        if (received <= 0) {
            return -1;
        }

        trim_crlf(line);
        if (strlen(line) >= 3 && line[0] >= '0' && line[0] <= '9' && line[1] >= '0' && line[1] <= '9' && line[1] <= '9' && line[2] >= '0' && line[2] <= '9') {
            int current_code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');

            if (expected_code < 0) {
                expected_code = current_code;
                multiline = (line[3] == '-');
            }

            if (response_text_size > 0) {
                snprintf(response_text, response_text_size, "%s", line);
            }

            printf("<-- %s\n", line);

            if (!multiline) {
                if (response_code != NULL) {
                    *response_code = current_code;
                }
                return 0;
            }

            if (current_code == expected_code && line[3] == ' ') {
                if (response_code != NULL) {
                    *response_code = current_code;
                }
                return 0;
            }
        } else {
            printf("<-- %s\n", line);
            if (response_text_size > 0) {
                snprintf(response_text, response_text_size, "%s", line);
            }
        }
    }
}

static int send_ftp_command(socket_t control_socket, const char *command) {
    printf("--> %s", command);
    return send_all(control_socket, command, strlen(command));
}

static socket_t connect_to_host(const char *host, const char *port) {
    struct addrinfo hints;
    struct addrinfo *results = NULL;
    struct addrinfo *current = NULL;
    socket_t sockfd = INVALID_SOCKET;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (getaddrinfo(host, port, &hints, &results) != 0) {
        return INVALID_SOCKET;
    }

    for (current = results; current != NULL; current = current->ai_next) {
        sockfd = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (sockfd == INVALID_SOCKET) {
            continue;
        }

        if (connect(sockfd, current->ai_addr, (int)current->ai_addrlen) == 0) {
            break;
        }

        close_socket(sockfd);
        sockfd = INVALID_SOCKET;
    }

    freeaddrinfo(results);
    return sockfd;
}

static int ftp_expect_code(socket_t control_socket, int expected_a, int expected_b, char *response_text, size_t response_text_size) {
    int response_code;
    if (read_ftp_response(control_socket, &response_code, response_text, response_text_size) < 0) {
        return -1;
    }

    if (response_code != expected_a && response_code != expected_b) {
        fprintf(stderr, "FTP error, expected %d or %d but got %d\n", expected_a, expected_b, response_code);
        return -1;
    }

    return response_code;
}

static int login_to_ftp(socket_t control_socket, const char *username, const char *password) {
    char command[256];
    char response[RESPONSE_SIZE];
    int response_code;

    if (ftp_expect_code(control_socket, 220, 220, response, sizeof(response)) < 0) {
        return -1;
    }

    snprintf(command, sizeof(command), "USER %s\r\n", username);
    if (send_ftp_command(control_socket, command) < 0) {
        return -1;
    }

    if (read_ftp_response(control_socket, &response_code, response, sizeof(response)) < 0) {
        return -1;
    }

    if (response_code == 230) {
        return 0;
    }
    if (response_code != 331) {
        fprintf(stderr, "Dang nhap USER that bai\n");
        return -1;
    }

    snprintf(command, sizeof(command), "PASS %s\r\n", password);
    if (send_ftp_command(control_socket, command) < 0) {
        return -1;
    }

    if (ftp_expect_code(control_socket, 230, 230, response, sizeof(response)) < 0) {
        return -1;
    }

    return 0;
}

static int set_binary_mode(socket_t control_socket) {
    char response[RESPONSE_SIZE];

    if (send_ftp_command(control_socket, "TYPE I\r\n") < 0) {
        return -1;
    }

    return ftp_expect_code(control_socket, 200, 200, response, sizeof(response)) < 0 ? -1 : 0;
}

static socket_t open_passive_data_socket(socket_t control_socket) {
    char response[RESPONSE_SIZE];
    const char *left_paren;
    int h1;
    int h2;
    int h3;
    int h4;
    int p1;
    int p2;
    char host[64];
    char port[16];

    if (send_ftp_command(control_socket, "PASV\r\n") < 0) {
        return INVALID_SOCKET;
    }

    if (ftp_expect_code(control_socket, 227, 227, response, sizeof(response)) < 0) {
        return INVALID_SOCKET;
    }

    left_paren = strchr(response, '(');
    if (left_paren == NULL) {
        fprintf(stderr, "Khong phan tich duoc phan hoi PASV\n");
        return INVALID_SOCKET;
    }

    if (sscanf(left_paren + 1, "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
        fprintf(stderr, "Khong doc duoc dia chi PASV\n");
        return INVALID_SOCKET;
    }

    snprintf(host, sizeof(host), "%d.%d.%d.%d", h1, h2, h3, h4);
    snprintf(port, sizeof(port), "%d", p1 * 256 + p2);
    return connect_to_host(host, port);
}

static int matches_question_name(const char *filename) {
    size_t length = strlen(filename);
    const char *prefix = "question_";
    const char *suffix = ".txt";
    size_t prefix_length = strlen(prefix);
    size_t suffix_length = strlen(suffix);

    if (length <= prefix_length + suffix_length) {
        return 0;
    }
    if (strncmp(filename, prefix, prefix_length) != 0) {
        return 0;
    }
    if (strcmp(filename + length - suffix_length, suffix) != 0) {
        return 0;
    }

    return 1;
}

static int ensure_directory_separator(char *path, size_t path_size) {
    size_t length = strlen(path);
    if (length == 0 || length + 1 >= path_size) {
        return -1;
    }

    if (path[length - 1] != '\\' && path[length - 1] != '/') {
        path[length] = '\\';
        path[length + 1] = '\0';
    }

    return 0;
}

static int get_output_directory(char *buffer, size_t buffer_size) {
#ifdef _WIN32
    DWORD length = GetModuleFileNameA(NULL, buffer, (DWORD)buffer_size);
    if (length == 0 || length >= buffer_size) {
        return -1;
    }

    while (length > 0 && buffer[length - 1] != '\\' && buffer[length - 1] != '/') {
        length--;
    }
    if (length == 0) {
        return -1;
    }

    buffer[length] = '\0';
    return 0;
#else
    if (snprintf(buffer, buffer_size, "./") >= (int)buffer_size) {
        return -1;
    }
    return 0;
#endif
}

static int build_path(char *output_path, size_t output_size, const char *directory, const char *filename) {
    if (snprintf(output_path, output_size, "%s%s", directory, filename) >= (int)output_size) {
        return -1;
    }
    return 0;
}

static int list_and_find_question(socket_t control_socket, char *question_name, size_t question_name_size) {
    socket_t data_socket;
    char response[RESPONSE_SIZE];
    char received_data[BUFFER_SIZE];
    char listing[BUFFER_SIZE * 2];
    size_t listing_used = 0;
    io_result_t received;
    char *line;
    char *context;

    question_name[0] = '\0';
    listing[0] = '\0';

    data_socket = open_passive_data_socket(control_socket);
    if (data_socket == INVALID_SOCKET) {
        return -1;
    }

    if (send_ftp_command(control_socket, "NLST\r\n") < 0) {
        close_socket(data_socket);
        return -1;
    }

    if (ftp_expect_code(control_socket, 125, 150, response, sizeof(response)) < 0) {
        close_socket(data_socket);
        return -1;
    }

    while ((received = recv(data_socket, received_data, sizeof(received_data) - 1, 0)) > 0) {
        if (listing_used + (size_t)received >= sizeof(listing)) {
            fprintf(stderr, "Danh sach file qua dai\n");
            close_socket(data_socket);
            return -1;
        }

        memcpy(listing + listing_used, received_data, (size_t)received);
        listing_used += (size_t)received;
        listing[listing_used] = '\0';
    }

    close_socket(data_socket);

    if (received == SOCKET_ERROR) {
        fprintf(stderr, "Khong nhan duoc danh sach file\n");
        return -1;
    }

    if (ftp_expect_code(control_socket, 226, 250, response, sizeof(response)) < 0) {
        return -1;
    }

    printf("Danh sach file tren FTP server:\n%s", listing);

    line = strtok_s(listing, "\r\n", &context);
    while (line != NULL) {
        if (matches_question_name(line)) {
            snprintf(question_name, question_name_size, "%s", line);
            return 0;
        }
        line = strtok_s(NULL, "\r\n", &context);
    }

    fprintf(stderr, "Khong tim thay file question_xxxxxx.txt\n");
    return -1;
}

static int download_file(socket_t control_socket, const char *remote_name, const char *local_path, unsigned char **file_data, size_t *file_size) {
    socket_t data_socket;
    char command[256];
    char response[RESPONSE_SIZE];
    unsigned char *buffer = NULL;
    size_t capacity = 0;
    size_t used = 0;
    io_result_t received;
    FILE *output;

    *file_data = NULL;
    *file_size = 0;

    data_socket = open_passive_data_socket(control_socket);
    if (data_socket == INVALID_SOCKET) {
        return -1;
    }

    snprintf(command, sizeof(command), "RETR %s\r\n", remote_name);
    if (send_ftp_command(control_socket, command) < 0) {
        close_socket(data_socket);
        return -1;
    }

    if (ftp_expect_code(control_socket, 125, 150, response, sizeof(response)) < 0) {
        close_socket(data_socket);
        return -1;
    }

    while (1) {
        unsigned char chunk[BUFFER_SIZE];
        received = recv(data_socket, (char *)chunk, sizeof(chunk), 0);
        if (received == SOCKET_ERROR) {
            close_socket(data_socket);
            free(buffer);
            return -1;
        }
        if (received == 0) {
            break;
        }

        if (used + (size_t)received > capacity) {
            size_t new_capacity = capacity == 0 ? (size_t)received * 2 : capacity * 2;
            unsigned char *new_buffer;

            while (new_capacity < used + (size_t)received) {
                new_capacity *= 2;
            }

            new_buffer = (unsigned char *)realloc(buffer, new_capacity);
            if (new_buffer == NULL) {
                close_socket(data_socket);
                free(buffer);
                return -1;
            }

            buffer = new_buffer;
            capacity = new_capacity;
        }

        memcpy(buffer + used, chunk, (size_t)received);
        used += (size_t)received;
    }

    close_socket(data_socket);

    if (ftp_expect_code(control_socket, 226, 250, response, sizeof(response)) < 0) {
        free(buffer);
        return -1;
    }

    output = fopen(local_path, "wb");
    if (output == NULL) {
        free(buffer);
        return -1;
    }

    if (used > 0 && fwrite(buffer, 1, used, output) != used) {
        fclose(output);
        free(buffer);
        return -1;
    }

    fclose(output);
    *file_data = buffer;
    *file_size = used;
    return 0;
}

static void reverse_bytes(unsigned char *data, size_t size) {
    size_t left = 0;
    size_t right = size == 0 ? 0 : size - 1;

    while (left < right) {
        unsigned char temp = data[left];
        data[left] = data[right];
        data[right] = temp;
        left++;
        right--;
    }
}

static int create_answer_name(const char *question_name, char *answer_name, size_t answer_size) {
    const char *prefix = "question_";
    const char *answer_prefix = "answer_";
    const char *suffix_part;

    if (strncmp(question_name, prefix, strlen(prefix)) != 0) {
        return -1;
    }

    suffix_part = question_name + strlen(prefix);
    if (snprintf(answer_name, answer_size, "%s%s", answer_prefix, suffix_part) >= (int)answer_size) {
        return -1;
    }

    return 0;
}

static int write_answer_file(const char *local_path, const unsigned char *data, size_t size) {
    FILE *output = fopen(local_path, "wb");
    if (output == NULL) {
        return -1;
    }

    if (size > 0 && fwrite(data, 1, size, output) != size) {
        fclose(output);
        return -1;
    }

    fclose(output);
    return 0;
}//yes//

static int upload_file(socket_t control_socket, const char *remote_name, const unsigned char *data, size_t size) {
    socket_t data_socket;
    char command[256];
    char response[RESPONSE_SIZE];

    data_socket = open_passive_data_socket(control_socket);
    if (data_socket == INVALID_SOCKET) {
        return -1;
    }

    snprintf(command, sizeof(command), "STOR %s\r\n", remote_name);
    if (send_ftp_command(control_socket, command) < 0) {
        close_socket(data_socket);
        return -1;
    }

    if (ftp_expect_code(control_socket, 125, 150, response, sizeof(response)) < 0) {
        close_socket(data_socket);
        return -1;
    }

    if (size > 0 && send_all(data_socket, data, size) < 0) {
        close_socket(data_socket);
        return -1;
    }

    shutdown_both(data_socket);
    close_socket(data_socket);

    return ftp_expect_code(control_socket, 226, 250, response, sizeof(response)) < 0 ? -1 : 0;
}

static void ftp_quit(socket_t control_socket) {
    char response[RESPONSE_SIZE];
    if (control_socket == INVALID_SOCKET) {
        return;
    }

    if (send_ftp_command(control_socket, "QUIT\r\n") == 0) {
        read_ftp_response(control_socket, NULL, response, sizeof(response));
    }
}

int main(void) {
    socket_t control_socket = INVALID_SOCKET;
    char output_directory[PATH_SIZE];
    char question_name[128];
    char answer_name[128];
    char question_path[PATH_SIZE];
    char answer_path[PATH_SIZE];
    unsigned char *question_data = NULL;
    size_t question_size = 0;
    int exit_code = EXIT_FAILURE;

    if (initialize_socket_system() < 0) {
        fprintf(stderr, "Khong khoi tao duoc Winsock\n");
        return EXIT_FAILURE;
    }

    if (get_output_directory(output_directory, sizeof(output_directory)) < 0) {
        fprintf(stderr, "Khong xac dinh duoc thu muc dau ra\n");
        cleanup_socket_system();
        return EXIT_FAILURE;
    }

    if (ensure_directory_separator(output_directory, sizeof(output_directory)) < 0) {
        fprintf(stderr, "Thu muc dau ra khong hop le\n");
        cleanup_socket_system();
        return EXIT_FAILURE;
    }

    printf("Dang ket noi FTP server %s:%s\n", FTP_HOST, FTP_PORT);
    control_socket = connect_to_host(FTP_HOST, FTP_PORT);
    if (control_socket == INVALID_SOCKET) {
        fprintf(stderr, "Khong ket noi duoc FTP server\n");
        cleanup_socket_system();
        return EXIT_FAILURE;
    }

    if (login_to_ftp(control_socket, FTP_USERNAME, FTP_PASSWORD) < 0) {
        goto cleanup;
    }

    if (set_binary_mode(control_socket) < 0) {
        goto cleanup;
    }

    if (list_and_find_question(control_socket, question_name, sizeof(question_name)) < 0) {
        goto cleanup;
    }

    printf("Da tim thay file cau hoi: %s\n", question_name);

    if (build_path(question_path, sizeof(question_path), output_directory, question_name) < 0) {
        fprintf(stderr, "Khong tao duoc duong dan file question\n");
        goto cleanup;
    }

    if (download_file(control_socket, question_name, question_path, &question_data, &question_size) < 0) {
        fprintf(stderr, "Tai file question that bai\n");
        goto cleanup;
    }

    printf("Da tai file ve: %s (%u bytes)\n", question_path, (unsigned int)question_size);

    if (create_answer_name(question_name, answer_name, sizeof(answer_name)) < 0) {
        fprintf(stderr, "Khong tao duoc ten file answer\n");
        goto cleanup;
    }

    reverse_bytes(question_data, question_size);

    if (build_path(answer_path, sizeof(answer_path), output_directory, answer_name) < 0) {
        fprintf(stderr, "Khong tao duoc duong dan file answer\n");
        goto cleanup;
    }

    if (write_answer_file(answer_path, question_data, question_size) < 0) {
        fprintf(stderr, "Khong ghi duoc file answer\n");
        goto cleanup;
    }

    printf("Da tao file tra loi: %s\n", answer_path);

    if (upload_file(control_socket, answer_name, question_data, question_size) < 0) {
        fprintf(stderr, "Upload file answer that bai\n");
        goto cleanup;
    }

    printf("Da upload file len server: %s\n", answer_name);
    exit_code = EXIT_SUCCESS;

cleanup:
    ftp_quit(control_socket);
    if (control_socket != INVALID_SOCKET) {
        close_socket(control_socket);
    }
    free(question_data);
    cleanup_socket_system();
    return exit_code;
}