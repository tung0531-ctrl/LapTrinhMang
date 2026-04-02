#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/*
 * sv_server
 * Cách dùng: sv_server <port> <log_file>
 *
 * Server nhận một struct sinh viên từ mỗi client, in ra màn hình,
 * đồng thời ghi thêm một dòng log gồm IP client, thời gian nhận và dữ liệu sinh viên.
 */

struct student_info {
    char mssv[32];
    char full_name[128];
    char birth_date[16];
    float gpa;
};

static int recv_all(int sockfd, void *buffer, size_t length) {
    char *data = (char *)buffer;
    size_t received_total = 0;

    /* Nhận cho đến khi đủ toàn bộ struct hoặc client đóng kết nối. */
    while (received_total < length) {
        ssize_t received_now = recv(sockfd, data + received_total, length - received_total, 0);
        if (received_now < 0) {
            return -1;
        }
        if (received_now == 0) {
            return 0;
        }
        received_total += (size_t)received_now;
    }

    return 1;
}

int main(int argc, char *argv[]) {
    /* Đề bài yêu cầu truyền vào cổng lắng nghe và tên file log. */
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <port> <log_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    /* Mở file log một lần, mỗi client nhận được sẽ ghi thêm một dòng. */
    FILE *log_file = fopen(argv[2], "a");
    if (log_file == NULL) {
        perror("fopen() failed");
        return EXIT_FAILURE;
    }

    /* Tạo socket TCP dùng để lắng nghe kết nối. */
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) {
        perror("socket() failed");
        fclose(log_file);
        return EXIT_FAILURE;
    }

    /* Cho phép tái sử dụng cổng nhanh sau khi chương trình dừng. */
    int opt = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt() failed");
        close(listener);
        fclose(log_file);
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
        fclose(log_file);
        return EXIT_FAILURE;
    }

    if (listen(listener, 5) < 0) {
        perror("listen() failed");
        close(listener);
        fclose(log_file);
        return EXIT_FAILURE;
    }

    printf("SV server listening on port %d\n", port);

    /* Xử lý lần lượt từng client và ghi một bản ghi cho mỗi lần kết nối. */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client = accept(listener, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client < 0) {
            perror("accept() failed");
            continue;
        }

        /* Xóa sạch struct trước khi nhận dữ liệu mới. */
        struct student_info student;
        memset(&student, 0, sizeof(student));

        /* Nhận đúng số byte tạo thành struct sinh viên. */
        int recv_status = recv_all(client, &student, sizeof(student));
        if (recv_status < 0) {
            perror("recv() failed");
            close(client);
            continue;
        }

        if (recv_status == 0) {
            close(client);
            continue;
        }

        /* Tạo chuỗi thời gian để in ra màn hình và ghi vào file log. */
        time_t now = time(NULL);
        struct tm *local = localtime(&now);
        char time_text[32];
        if (local == NULL || strftime(time_text, sizeof(time_text), "%Y-%m-%d %H:%M:%S", local) == 0) {
            strcpy(time_text, "1970-01-01 00:00:00");
        }

        /* Lấy địa chỉ IP của client vừa kết nối. */
        const char *client_ip = inet_ntoa(client_addr.sin_addr);

         /* In đúng một dòng cho mỗi client, cùng định dạng với file log. */
         printf("%s %s %s %s %s %.2f\n",
             client_ip,
             time_text,
             student.mssv,
             student.full_name,
             student.birth_date,
             student.gpa);

        /* Ghi thêm một dòng log đầy đủ theo yêu cầu đề bài. */
        fprintf(log_file,
                "%s %s %s %s %s %.2f\n",
                client_ip,
                time_text,
                student.mssv,
                student.full_name,
                student.birth_date,
                student.gpa);
        fflush(log_file);

        close(client);
    }

    close(listener);
    fclose(log_file);
    return EXIT_SUCCESS;
}