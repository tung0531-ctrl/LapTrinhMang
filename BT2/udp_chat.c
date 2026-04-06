#include <arpa/inet.h>
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

#define BUFFER_SIZE 2048

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

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <port_s> <ip_d> <port_d>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int source_port = atoi(argv[1]);
    int destination_port = atoi(argv[3]);
    if (source_port <= 0 || source_port > 65535 || destination_port <= 0 || destination_port > 65535) {
        fprintf(stderr, "Port khong hop le\n");
        return EXIT_FAILURE;
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        perror("socket() failed");
        return EXIT_FAILURE;
    }

    if (set_non_blocking(sockfd) < 0) {
        perror("fcntl() failed");
        close(sockfd);
        return EXIT_FAILURE;
    }

    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons((unsigned short)source_port);

    if (bind(sockfd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        perror("bind() failed");
        close(sockfd);
        return EXIT_FAILURE;
    }

    struct sockaddr_in destination_addr;
    memset(&destination_addr, 0, sizeof(destination_addr));
    destination_addr.sin_family = AF_INET;
    destination_addr.sin_port = htons((unsigned short)destination_port);
    if (inet_pton(AF_INET, argv[2], &destination_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", argv[2]);
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("UDP chat dang chay tai cong %d\n", source_port);
    printf("Dich mac dinh: %s:%d\n", argv[2], destination_port);
    printf("Nhap noi dung va nhan Enter de gui. Nhan Ctrl+D de thoat.\n");

    while (1) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(sockfd, &read_set);
        FD_SET(STDIN_FILENO, &read_set);

        int max_fd = sockfd > STDIN_FILENO ? sockfd : STDIN_FILENO;
        if (select(max_fd + 1, &read_set, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select() failed");
            break;
        }

        if (FD_ISSET(sockfd, &read_set)) {
            while (1) {
                char buffer[BUFFER_SIZE];
                struct sockaddr_in peer_addr;
                socklen_t peer_length = sizeof(peer_addr);
                ssize_t received = recvfrom(sockfd,
                                            buffer,
                                            sizeof(buffer) - 1,
                                            0,
                                            (struct sockaddr *)&peer_addr,
                                            &peer_length);

                if (received < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    perror("recvfrom() failed");
                    close(sockfd);
                    return EXIT_FAILURE;
                }

                buffer[received] = '\0';
                printf("\n[%s:%d] %s",
                       inet_ntoa(peer_addr.sin_addr),
                       ntohs(peer_addr.sin_port),
                       buffer);
                if (received == 0 || buffer[received - 1] != '\n') {
                    printf("\n");
                }
                printf("> ");
                fflush(stdout);
            }
        }

        if (FD_ISSET(STDIN_FILENO, &read_set)) {
            char buffer[BUFFER_SIZE];
            if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
                printf("\nKet thuc chat.\n");
                break;
            }

            ssize_t sent = sendto(sockfd,
                                  buffer,
                                  strlen(buffer),
                                  0,
                                  (struct sockaddr *)&destination_addr,
                                  sizeof(destination_addr));
            if (sent < 0) {
                perror("sendto() failed");
                close(sockfd);
                return EXIT_FAILURE;
            }

            printf("> ");
            fflush(stdout);
        }
    }

    close(sockfd);
    return EXIT_SUCCESS;
}