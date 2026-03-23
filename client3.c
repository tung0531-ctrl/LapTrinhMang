/*******************************************************************************
 * @File: client3.c
 * @Date: 2026-03-17
 * @Description: Client kết nối đến http://httpbin.get/org
 *******************************************************************************/

#define _XOPEN_SOURCE 700

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
    int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client < 0) {
        perror("socket() failed");
        exit(EXIT_FAILURE);
    }

    // Phân giải tên miền httpbin.org
    struct addrinfo *res;
    if (getaddrinfo("httpbin.org", "http", NULL, &res)) {
        printf("getaddrinfo() failed");
        exit(EXIT_FAILURE);
    }
    
    if (connect(client, res->ai_addr, res->ai_addrlen)) {
        perror("connect() failed");
        exit(EXIT_FAILURE);
    }

    char buf[2048] = "GET /get HTTP/1.1\r\nHost: httpbin.org\r\n\r\n";
    send(client, buf, strlen(buf), 0);
    
    int ret = recv(client, buf, sizeof(buf), 0);
    buf[ret] = 0;
    printf("%s\n", buf);

    close(client);

    return 0;
}