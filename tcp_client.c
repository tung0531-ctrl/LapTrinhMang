#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * tcp_client
 * Cách dùng: tcp_client <ip> <port>
 *
 * Chương trình client kết nối tới server, có thể nhận câu chào
 * từ server, sau đó đọc dữ liệu từ bàn phím và gửi đi.
 */

static void send_all(int sockfd, const char *buffer, size_t length) {
    size_t sent_total = 0;

    /* Gửi cho đến khi toàn bộ dữ liệu trong bộ đệm đã được chuyển hết. */
    while (sent_total < length) {
        ssize_t sent_now = send(sockfd, buffer + sent_total, length - sent_total, 0);
        if (sent_now < 0) {
            perror("send() failed");
            close(sockfd);
            exit(EXIT_FAILURE);
        }
        sent_total += (size_t)sent_now;
    }
}

int main(int argc, char *argv[]) {
    /* Đề bài yêu cầu địa chỉ IP và cổng được truyền từ dòng lệnh. */
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <ip> <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    /* Tạo socket TCP để kết nối tới server. */
    int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client < 0) {
        perror("socket() failed");
        return EXIT_FAILURE;
    }

    /* Điền thông tin địa chỉ IPv4 từ tham số dòng lệnh. */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);

    if (inet_pton(AF_INET, argv[1], &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", argv[1]);
        close(client);
        return EXIT_FAILURE;
    }

    /* Thực hiện kết nối TCP tới server. */
    if (connect(client, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect() failed");
        close(client);
        return EXIT_FAILURE;
    }

    /*
     * Chờ trong thời gian ngắn để nhận câu chào từ server.
     * Đặt timeout để client không bị treo vô hạn nếu server không gửi dữ liệu.
     */
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt() failed");
        close(client);
        return EXIT_FAILURE;
    }

    /* Nếu server gửi câu chào thì hiển thị ra màn hình trước khi gửi dữ liệu. */
    char greeting[1024];
    ssize_t greeting_len = recv(client, greeting, sizeof(greeting) - 1, 0);
    if (greeting_len > 0) {
        greeting[greeting_len] = '\0';
        printf("Server greeting:\n%s", greeting);
        if (greeting[greeting_len - 1] != '\n') {
            printf("\n");
        }
    }

    /* Đọc từng dòng từ bàn phím và gửi sang server. */
    char buffer[1024];
    printf("Nhap chuoi can gui\n");
   
    fflush(stdout);
    while (fgets(buffer, sizeof(buffer), stdin) != NULL) {
        send_all(client, buffer, strlen(buffer));
        
        fflush(stdout);
    }

    close(client);
    return EXIT_SUCCESS;
}