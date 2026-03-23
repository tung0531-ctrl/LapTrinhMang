/*******************************************************************************
 * @File: client5.c
 * @Date: 2026-03-17
 * @Description: Client truyền file
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
    int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client < 0) {
        perror("socket() failed");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(9000);

    if (connect(client, (struct sockaddr *)&addr, sizeof(addr))) {
        perror("connect() failed");
        exit(EXIT_FAILURE);
    }

    FILE *f = fopen("test.pdf", "rb");

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Truyen kich thuoc file
    send(client, &size, sizeof(size), 0);

    char buf[2048];

    long progress = 0;

    // Doc và truyen noi dung file
    while (1) {
        int len = fread(buf, 1, sizeof(buf), f);
        if (len <= 0)
            break;
        send(client, buf, len, 0);
        progress += len;
        printf("\rProgress: %ld/%ld", progress, size);
    }

    fclose(f);
    close(client);

    return 0;
}