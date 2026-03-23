/*******************************************************************************
 * @File: server3.c
 * @Date: 2026-03-17
 * @Description: Server nhận dữ liệu từ trình duyệt
 *******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

int main() {
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(9090);

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr))) {
        perror("bind() failed");
        exit(EXIT_FAILURE);
    }

    listen(listener, 5);

    char buf[2048];

    while (1) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            perror("accept() failed");
            continue;
        }

        int len = recv(client, buf, sizeof(buf), 0);
        if (len <= 0) {
            continue;
        }

        buf[len] = 0;
        printf("Received: %s\n", buf);

        // Gửi phản hồi cho trình duyệt
        char msg[] = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<html><body><h1>Hello World</h1></body></html>";
        send(client, msg, strlen(msg), 0);
        
        close(client);
    }

    close(listener);

    return 0;
}