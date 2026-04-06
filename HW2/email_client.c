#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int recv_line(int sockfd, char *buffer, size_t buffer_size) {
    size_t index = 0;

    while (index + 1 < buffer_size) {
        ssize_t received = recv(sockfd, &buffer[index], 1, 0);
        if (received < 0) {
            perror("recv() failed");
            return -1;
        }
        if (received == 0) {
            break;
        }

        if (buffer[index] == '\n') {
            index++;
            break;
        }
        index++;
    }

    buffer[index] = '\0';
    return (int)index;
}

static int recv_prompt(int sockfd, char *buffer, size_t buffer_size) {
    size_t index = 0;

    while (index + 1 < buffer_size) {
        ssize_t received = recv(sockfd, &buffer[index], 1, 0);
        if (received < 0) {
            perror("recv() failed");
            return -1;
        }
        if (received == 0) {
            break;
        }

        if (buffer[index] == ':') {
            index++;
            if (index + 1 < buffer_size) {
                received = recv(sockfd, &buffer[index], 1, 0);
                if (received > 0) {
                    index++;
                }
            }
            break;
        }

        index++;
    }

    buffer[index] = '\0';
    return (int)index;
}

static void send_text(int sockfd, const char *text) {
    size_t sent_total = 0;
    size_t length = strlen(text);

    while (sent_total < length) {
        ssize_t sent = send(sockfd, text + sent_total, length - sent_total, 0);
        if (sent < 0) {
            perror("send() failed");
            close(sockfd);
            exit(EXIT_FAILURE);
        }
        sent_total += (size_t)sent;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <ip> <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client < 0) {
        perror("socket() failed");
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((unsigned short)port);

    if (inet_pton(AF_INET, argv[1], &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", argv[1]);
        close(client);
        return EXIT_FAILURE;
    }

    if (connect(client, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect() failed");
        close(client);
        return EXIT_FAILURE;
    }

    char prompt[256];
    char input[256];
    char response[256];

    if (recv_prompt(client, prompt, sizeof(prompt)) <= 0) {
        fprintf(stderr, "Server dong ket noi som\n");
        close(client);
        return EXIT_FAILURE;
    }
    printf("%s", prompt);
    if (fgets(input, sizeof(input), stdin) == NULL) {
        close(client);
        return EXIT_FAILURE;
    }
    send_text(client, input);

    if (recv_prompt(client, prompt, sizeof(prompt)) <= 0) {
        fprintf(stderr, "Khong nhan duoc prompt MSSV\n");
        close(client);
        return EXIT_FAILURE;
    }
    printf("%s", prompt);
    if (fgets(input, sizeof(input), stdin) == NULL) {
        close(client);
        return EXIT_FAILURE;
    }
    send_text(client, input);

    if (recv_line(client, response, sizeof(response)) > 0) {
        printf("%s", response);
    }

    close(client);
    return EXIT_SUCCESS;
}