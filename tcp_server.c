#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * tcp_server
 * Cách dùng: tcp_server <port> <greeting_file> <output_file>
 *
 * Chương trình server lắng nghe tại cổng được chỉ định, gửi nội dung câu chào
 * đọc từ file cho mỗi client, sau đó ghi nội dung client gửi đến vào file kết quả.
 */

static char *read_file_content(const char *file_name, size_t *content_size) {
    FILE *file = fopen(file_name, "rb");
    char *content;
    long size;

    /* Đọc nội dung file câu chào một lần để dùng lại cho mỗi client. */
    if (file == NULL) {
        perror("fopen() failed");
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        perror("fseek() failed");
        fclose(file);
        return NULL;
    }

    size = ftell(file);
    if (size < 0) {
        perror("ftell() failed");
        fclose(file);
        return NULL;
    }

    rewind(file);

    content = (char *)malloc((size_t)size + 1);
    if (content == NULL) {
        fprintf(stderr, "malloc() failed\n");
        fclose(file);
        return NULL;
    }

    if (size > 0 && fread(content, 1, (size_t)size, file) != (size_t)size) {
        perror("fread() failed");
        free(content);
        fclose(file);
        return NULL;
    }

    content[size] = '\0';
    *content_size = (size_t)size;

    fclose(file);
    return content;
}

static void send_all(int sockfd, const char *buffer, size_t length) {
    size_t sent_total = 0;

    /* Gửi cho đến khi toàn bộ câu chào đã được chuyển xong. */
    while (sent_total < length) {
        ssize_t sent_now = send(sockfd, buffer + sent_total, length - sent_total, 0);
        if (sent_now < 0) {
            perror("send() failed");
            return;
        }
        sent_total += (size_t)sent_now;
    }
}

int main(int argc, char *argv[]) {
    /* Đề bài yêu cầu truyền vào cổng, file câu chào và file lưu kết quả. */
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <port> <greeting_file> <output_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    /* Đọc file câu chào trước khi bắt đầu vòng lặp nhận client. */
    size_t greeting_size = 0;
    char *greeting = read_file_content(argv[2], &greeting_size);
    if (greeting == NULL) {
        return EXIT_FAILURE;
    }

    /* Mở file kết quả ở chế độ nối thêm để giữ lại dữ liệu của các lần kết nối. */
    FILE *output = fopen(argv[3], "ab");
    if (output == NULL) {
        perror("fopen() failed");
        free(greeting);
        return EXIT_FAILURE;
    }

    /* Tạo socket TCP dùng để lắng nghe. */
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) {
        perror("socket() failed");
        fclose(output);
        free(greeting);
        return EXIT_FAILURE;
    }

    /* Cho phép mở lại nhanh cùng cổng sau khi chương trình kết thúc. */
    int opt = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt() failed");
        close(listener);
        fclose(output);
        free(greeting);
        return EXIT_FAILURE;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind() failed");
        close(listener);
        fclose(output);
        free(greeting);
        return EXIT_FAILURE;
    }

    if (listen(listener, 5) < 0) {
        perror("listen() failed");
        close(listener);
        fclose(output);
        free(greeting);
        return EXIT_FAILURE;
    }

    printf("Listening on port %d\n", port);

    /* Xử lý từng client trong vòng lặp chấp nhận kết nối liên tục. */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client = accept(listener, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client < 0) {
            perror("accept() failed");
            continue;
        }

        printf("Client connected from %s:%d\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));

        /* Gửi nội dung câu chào theo yêu cầu đề bài. */
        if (greeting_size > 0) {
            send_all(client, greeting, greeting_size);
        }

        /* Nhận dữ liệu từ client và ghi nối thêm trực tiếp vào file kết quả. */
        char buffer[1024];
        while (1) {
            ssize_t received = recv(client, buffer, sizeof(buffer), 0);
            if (received < 0) {
                perror("recv() failed");
                break;
            }
            if (received == 0) {
                break;
            }

            printf("Da nhan %zd byte: ", received);
            fwrite(buffer, 1, (size_t)received, stdout);
            if (buffer[received - 1] != '\n') {
                printf("\n");
            }

            if (fwrite(buffer, 1, (size_t)received, output) != (size_t)received) {
                perror("fwrite() failed");
                break;
            }
            fflush(output);
        }

        /* Đóng kết nối hiện tại và quay lại chờ client tiếp theo. */
        close(client);
        printf("Client disconnected\n");
    }

    close(listener);
    fclose(output);
    free(greeting);
    return EXIT_SUCCESS;
}