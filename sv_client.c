#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * sv_client
 * Cách dùng: sv_client <ip> <port>
 *
 * Người dùng nhập thông tin của một sinh viên.
 * Thông tin này được đóng gói vào struct và gửi sang server trong một kết nối TCP.
 */

struct student_info {
    char mssv[32];
    char full_name[128];
    char birth_date[16];
    float gpa;
};

static void trim_newline(char *text) {
    /* fgets giữ lại ký tự xuống dòng, cần xóa đi trước khi lưu. */
    size_t len = strlen(text);
    if (len > 0 && text[len - 1] == '\n') {
        text[len - 1] = '\0';
    }
}

static void send_all(int sockfd, const void *buffer, size_t length) {
    const char *data = (const char *)buffer;
    size_t sent_total = 0;

    /* Gửi cho đến khi toàn bộ struct sinh viên đã được chuyển hết. */
    while (sent_total < length) {
        ssize_t sent_now = send(sockfd, data + sent_total, length - sent_total, 0);
        if (sent_now < 0) {
            perror("send() failed");
            close(sockfd);
            exit(EXIT_FAILURE);
        }
        sent_total += (size_t)sent_now;
    }
}

int main(int argc, char *argv[]) {
    /* Đề bài yêu cầu IP và cổng của server được truyền từ dòng lệnh. */
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <ip> <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    /* Nhập đầy đủ các trường thông tin sinh viên theo đề bài. */
    struct student_info student;
    memset(&student, 0, sizeof(student));

    printf("MSSV: ");
    if (fgets(student.mssv, sizeof(student.mssv), stdin) == NULL) {
        fprintf(stderr, "Failed to read MSSV\n");
        return EXIT_FAILURE;
    }
    trim_newline(student.mssv);

    printf("Ho ten: ");
    if (fgets(student.full_name, sizeof(student.full_name), stdin) == NULL) {
        fprintf(stderr, "Failed to read full name\n");
        return EXIT_FAILURE;
    }
    trim_newline(student.full_name);

    printf("Ngay sinh (YYYY-MM-DD): ");
    if (fgets(student.birth_date, sizeof(student.birth_date), stdin) == NULL) {
        fprintf(stderr, "Failed to read birth date\n");
        return EXIT_FAILURE;
    }
    trim_newline(student.birth_date);

    char gpa_buffer[32];
    printf("Diem trung binh: ");
    if (fgets(gpa_buffer, sizeof(gpa_buffer), stdin) == NULL) {
        fprintf(stderr, "Failed to read GPA\n");
        return EXIT_FAILURE;
    }
    student.gpa = strtof(gpa_buffer, NULL);

    /* Tạo socket và kết nối tới sv_server. */
    int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client < 0) {
        perror("socket() failed");
        return EXIT_FAILURE;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);

    if (inet_pton(AF_INET, argv[1], &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", argv[1]);
        close(client);
        return EXIT_FAILURE;
    }

    if (connect(client, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect() failed");
        close(client);
        return EXIT_FAILURE;
    }

    /* Gửi toàn bộ struct một lần rồi kết thúc phiên làm việc. */
    send_all(client, &student, sizeof(student));
    close(client);

    return EXIT_SUCCESS;
}