/*******************************************************************************
 * @File: client2.c
 * @Date: 2026-03-17
 * @Description: Client truyền dữ liệu bất kỳ
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

    struct sinhvien sv;

    while (1) {
        printf("Enter mssv: ");
        fflush(stdin);scanf("%d", &sv.mssv);
        printf("Enter hoten: ");
        fflush(stdin);scanf("%s", sv.hoten);
        printf("Enter tuoi: ");
        fflush(stdin);scanf("%d", &sv.tuoi);

        send(client, &sv, sizeof(sv), 0);
        if (sv.tuoi == 0)
            break;
    }

    close(client);

    return 0;
}