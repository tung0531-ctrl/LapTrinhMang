/*******************************************************************************
 * @File: server4.c
 * @Date: 2026-03-17
 * @Description: Server nhận dữ liệu bất kỳ
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

struct sinhvien {
    int mssv;
    char hoten[64];
    int tuoi;
};

int main() {
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(9000);

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr))) {
        perror("bind() failed");
        exit(EXIT_FAILURE);
    }

    listen(listener, 5);

    int client = accept(listener, NULL, NULL);
    if (client < 0) {
        perror("accept() failed");
        exit(EXIT_FAILURE);
    }

    struct sinhvien sv;
    while (1) {
        int ret = recv(client, &sv, sizeof(sv), 0);
        if (ret <= 0)
            break;

        printf("Received:\n%d\n%s\n%d\n", sv.mssv, sv.hoten, sv.tuoi);
    }

    close(client);
    close(listener);

    return 0;
}