#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

SOCKET s;
int go = 1;

DWORD WINAPI recv_func(LPVOID param) {
    char buf[2048];
    int res;
    while (go) {
        res = recv(s, buf, 2047, 0);
        if (res > 0) {
            buf[res] = 0;
            printf("%s", buf);
        }
        else if (res == 0) {
            printf("\ndisconnected\n");
            go = 0;
            break;
        }
        else {
            int er = WSAGetLastError();
            if (er != WSAEWOULDBLOCK) {
                go = 0;
                break;
            }
        }
    }
    return 0;
}

int main(int argc, char* argv[]) {
    WSADATA w;
    struct sockaddr_in a;

    if (argc < 2) {
        printf("usage: client.exe <ip>\n");
        return 1;
    }

    WSAStartup(MAKEWORD(2, 2), &w);

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        return 1;
    }

    a.sin_family = AF_INET;
    a.sin_port = htons(9999);
    inet_pton(AF_INET, argv[1], &a.sin_addr);

    printf("connecting to %s...\n", argv[1]);
    int conn_res = connect(s, (struct sockaddr*)&a, sizeof(a));
    if (conn_res != 0) {
        printf("connection failed\n");
        return 1;
    }

    printf("connected\n\n");

    HANDLE th = CreateThread(NULL, 0, recv_func, NULL, 0, NULL);

    char input[1024];
    while (go == 1) {
        if (fgets(input, 1024, stdin)) {
            int len = strlen(input);
            send(s, input, len, 0);
        }
    }

    closesocket(s);
    if (th != NULL) {
        WaitForSingleObject(th, 2000);
        CloseHandle(th);
    }
    WSACleanup();

    return 0;
}
