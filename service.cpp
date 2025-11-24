#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <stdio.h>
#include <vector>
#include <time.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")

SERVICE_STATUS svc_stat;
SERVICE_STATUS_HANDLE svc_handle;
HANDLE stop_evt;
LONG run_flag = 1;
SOCKET listen_s = INVALID_SOCKET;
CRITICAL_SECTION cs;
HANDLE monitor_thread = NULL;

#define MAX_SESS 16
#define LOG_FILE "C:\\RemoteConsole\\service.log"

struct ClientSess {
    SOCKET sock;
    HANDLE proc_h;
    HANDLE proc_t;
    HANDLE pipe_w;
    HANDLE pipe_r;
    HANDLE thread1;
    HANDLE thread2;
    LONG active;
    DWORD thread1_id;
    DWORD thread2_id;
    time_t start_time;
};

std::vector<ClientSess*> clients;

void WriteLog(const char* msg) {
    FILE* f = fopen(LOG_FILE, "a");
    if (f) {
        time_t now = time(NULL);
        char timebuf[64];
        struct tm* tm_info = localtime(&now);
        strftime(timebuf, 64, "%Y-%m-%d %H:%M:%S", tm_info);
        fprintf(f, "[%s] %s\n", timebuf, msg);
        fclose(f);
    }
}

void SetStatus(DWORD state, DWORD code, DWORD wait) {
    static DWORD checkpoint = 1;
    svc_stat.dwCurrentState = state;
    svc_stat.dwWin32ExitCode = code;
    svc_stat.dwWaitHint = wait;

    if (state == SERVICE_START_PENDING)
        svc_stat.dwControlsAccepted = 0;
    else
        svc_stat.dwControlsAccepted = SERVICE_ACCEPT_STOP;

    if (state == SERVICE_RUNNING || state == SERVICE_STOPPED) {
        svc_stat.dwCheckPoint = 0;
    }
    else {
        svc_stat.dwCheckPoint = checkpoint++;
    }

    SetServiceStatus(svc_handle, &svc_stat);
}

VOID WINAPI ServiceCtrl(DWORD ctrl) {
    if (ctrl == SERVICE_CONTROL_STOP) {
        WriteLog("Service stop requested");
        SetStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);
        InterlockedExchange(&run_flag, 0);
        SetEvent(stop_evt);

        if (listen_s != INVALID_SOCKET) {
            shutdown(listen_s, SD_BOTH);
            closesocket(listen_s);
            listen_s = INVALID_SOCKET;
        }

        EnterCriticalSection(&cs);
        for (size_t i = 0; i < clients.size(); i++) {
            if (clients[i] && clients[i]->sock != INVALID_SOCKET) {
                shutdown(clients[i]->sock, SD_BOTH);
                closesocket(clients[i]->sock);
                clients[i]->sock = INVALID_SOCKET;
            }
        }
        LeaveCriticalSection(&cs);
    }
}

DWORD WINAPI RecvThread(LPVOID arg) {
    ClientSess* sess = (ClientSess*)arg;
    char data[4096];
    DWORD written;

    while (InterlockedCompareExchange(&sess->active, 0, 0) &&
        InterlockedCompareExchange(&run_flag, 0, 0)) {

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sess->sock, &readfds);

        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        int sel_res = select(0, &readfds, NULL, NULL, &tv);

        if (sel_res == SOCKET_ERROR) break;
        if (sel_res == 0) {
            if (WaitForSingleObject(stop_evt, 0) == WAIT_OBJECT_0)
                break;
            continue;
        }

        if (FD_ISSET(sess->sock, &readfds)) {
            int bytes = recv(sess->sock, data, 4096, 0);
            if (bytes > 0) {
                WriteFile(sess->pipe_w, data, bytes, &written, NULL);
            }
            else if (bytes == 0) {
                break;
            }
            else {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK) continue;
                break;
            }
        }
    }

    char logmsg[256];
    sprintf(logmsg, "RecvThread (ID:%d) exited", GetCurrentThreadId());
    WriteLog(logmsg);

    return 0;
}

DWORD WINAPI SendThread(LPVOID arg) {
    ClientSess* sess = (ClientSess*)arg;
    char buffer[4096];
    DWORD bytes_read;

    while (InterlockedCompareExchange(&sess->active, 0, 0) &&
        InterlockedCompareExchange(&run_flag, 0, 0)) {

        DWORD available = 0;
        if (!PeekNamedPipe(sess->pipe_r, NULL, 0, NULL, &available, NULL)) {
            break;
        }

        if (available > 0) {
            if (ReadFile(sess->pipe_r, buffer, 4096, &bytes_read, NULL) && bytes_read > 0) {
                int sent = send(sess->sock, buffer, bytes_read, 0);
                if (sent == SOCKET_ERROR) {
                    int err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK) {
                        Sleep(50);
                        continue;
                    }
                    break;
                }
            }
        }
        else {
            if (WaitForSingleObject(stop_evt, 100) == WAIT_OBJECT_0)
                break;
        }
    }

    char logmsg[256];
    sprintf(logmsg, "SendThread (ID:%d) exited", GetCurrentThreadId());
    WriteLog(logmsg);

    return 0;
}

void CleanupSession(ClientSess* s) {
    if (!s) return;

    char logmsg[256];
    sprintf(logmsg, "Cleaning up session (threads: %d, %d)", s->thread1_id, s->thread2_id);
    WriteLog(logmsg);

    InterlockedExchange(&s->active, 0);

    if (s->sock != INVALID_SOCKET) {
        shutdown(s->sock, SD_BOTH);
        closesocket(s->sock);
        s->sock = INVALID_SOCKET;
    }

    if (s->thread1) {
        WaitForSingleObject(s->thread1, 5000);
        CloseHandle(s->thread1);
    }

    if (s->thread2) {
        WaitForSingleObject(s->thread2, 5000);
        CloseHandle(s->thread2);
    }

    if (s->pipe_w) CloseHandle(s->pipe_w);
    if (s->pipe_r) CloseHandle(s->pipe_r);

    if (s->proc_h) {
        if (WaitForSingleObject(s->proc_h, 2000) == WAIT_TIMEOUT) {
            TerminateProcess(s->proc_h, 0);
            WaitForSingleObject(s->proc_h, 2000);
        }
        CloseHandle(s->proc_h);
    }

    if (s->proc_t) CloseHandle(s->proc_t);

    delete s;
}

DWORD WINAPI MonitorThread(LPVOID arg) {
    WriteLog("Monitor thread started");

    while (InterlockedCompareExchange(&run_flag, 0, 0)) {
        if (WaitForSingleObject(stop_evt, 0) == WAIT_OBJECT_0) {
            break;
        }

        EnterCriticalSection(&cs);

        for (size_t i = 0; i < clients.size(); ) {
            ClientSess* sess = clients[i];

            if (!sess || !InterlockedCompareExchange(&sess->active, 0, 0)) {
                i++;
                continue;
            }

            DWORD exit_code1 = STILL_ACTIVE;
            DWORD exit_code2 = STILL_ACTIVE;

            if (sess->thread1) {
                GetExitCodeThread(sess->thread1, &exit_code1);
            }

            if (sess->thread2) {
                GetExitCodeThread(sess->thread2, &exit_code2);
            }

            bool t1_dead = (exit_code1 != STILL_ACTIVE);
            bool t2_dead = (exit_code2 != STILL_ACTIVE);

            if (t1_dead || t2_dead) {
                char logmsg[256];
                time_t now = time(NULL);
                time_t uptime = now - sess->start_time;

                sprintf(logmsg, "UNEXPECTED THREAD DEATH detected! Thread1:%s Thread2:%s (uptime: %d sec)",
                    t1_dead ? "DEAD" : "alive",
                    t2_dead ? "DEAD" : "alive",
                    (int)uptime);
                WriteLog(logmsg);

                InterlockedExchange(&sess->active, 0);

                if (sess->sock != INVALID_SOCKET) {
                    closesocket(sess->sock);
                    sess->sock = INVALID_SOCKET;
                }

                CleanupSession(sess);

                clients.erase(clients.begin() + i);

                WriteLog("Dead session removed from list");
                continue;
            }

            i++;
        }

        LeaveCriticalSection(&cs);

        Sleep(2000);
    }

    WriteLog("Monitor thread exiting");
    return 0;
}

VOID WINAPI SvcMain(DWORD argc, LPSTR* argv) {
    WriteLog("Service starting");

    svc_handle = RegisterServiceCtrlHandlerA("RemoteConsoleSvc", ServiceCtrl);
    if (!svc_handle) return;

    svc_stat.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    svc_stat.dwServiceSpecificExitCode = 0;

    SetStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    stop_evt = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!stop_evt) {
        SetStatus(SERVICE_STOPPED, NO_ERROR, 0);
        return;
    }

    InitializeCriticalSection(&cs);

    SetStatus(SERVICE_RUNNING, NO_ERROR, 0);

    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) {
        SetStatus(SERVICE_STOPPED, NO_ERROR, 0);
        return;
    }

    listen_s = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_s == INVALID_SOCKET) goto cleanup;

    int opt = 1;
    setsockopt(listen_s, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(9999);

    if (bind(listen_s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
        goto cleanup;

    if (listen(listen_s, SOMAXCONN) == SOCKET_ERROR)
        goto cleanup;

    u_long mode = 1;
    ioctlsocket(listen_s, FIONBIO, &mode);

    monitor_thread = CreateThread(NULL, 0, MonitorThread, NULL, 0, NULL);
    WriteLog("Monitor thread created");

    while (InterlockedCompareExchange(&run_flag, 0, 0)) {
        if (WaitForSingleObject(stop_evt, 0) == WAIT_OBJECT_0)
            break;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen_s, &fds);

        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 500000;

        int res = select(0, &fds, NULL, NULL, &timeout);
        if (res == SOCKET_ERROR) break;
        if (res == 0) continue;

        if (FD_ISSET(listen_s, &fds)) {
            sockaddr_in client_addr;
            int len = sizeof(client_addr);
            SOCKET new_s = accept(listen_s, (sockaddr*)&client_addr, &len);

            if (new_s == INVALID_SOCKET) {
                int e = WSAGetLastError();
                if (e == WSAEWOULDBLOCK) continue;
                continue;
            }

            WriteLog("New client connected");

            EnterCriticalSection(&cs);
            int cnt = clients.size();
            if (cnt >= MAX_SESS) {
                LeaveCriticalSection(&cs);
                char* msg = "server full\r\n";
                send(new_s, msg, strlen(msg), 0);
                shutdown(new_s, SD_BOTH);
                closesocket(new_s);
                WriteLog("Client rejected - server full");
                continue;
            }
            LeaveCriticalSection(&cs);

            SECURITY_ATTRIBUTES sa;
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;
            sa.lpSecurityDescriptor = NULL;

            HANDLE in_r, in_w, out_r, out_w;

            if (!CreatePipe(&in_r, &in_w, &sa, 0)) {
                closesocket(new_s);
                continue;
            }
            SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);

            if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
                CloseHandle(in_r);
                CloseHandle(in_w);
                closesocket(new_s);
                continue;
            }
            SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOA si;
            PROCESS_INFORMATION pi;
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(STARTUPINFOA);
            si.hStdInput = in_r;
            si.hStdOutput = out_w;
            si.hStdError = out_w;
            si.dwFlags = STARTF_USESTDHANDLES;

            char cmd[] = "cmd.exe";
            BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                CREATE_NO_WINDOW, NULL, NULL,
                &si, &pi);

            if (!ok) {
                CloseHandle(in_r);
                CloseHandle(in_w);
                CloseHandle(out_r);
                CloseHandle(out_w);
                closesocket(new_s);
                WriteLog("Failed to create cmd.exe process");
                continue;
            }

            CloseHandle(in_r);
            CloseHandle(out_w);

            ClientSess* sess = new ClientSess();
            sess->sock = new_s;
            sess->proc_h = pi.hProcess;
            sess->proc_t = pi.hThread;
            sess->pipe_w = in_w;
            sess->pipe_r = out_r;
            sess->start_time = time(NULL);
            InterlockedExchange(&sess->active, 1);

            u_long sock_mode = 1;
            ioctlsocket(new_s, FIONBIO, &sock_mode);

            sess->thread1 = CreateThread(NULL, 0, RecvThread, sess, 0, &sess->thread1_id);
            sess->thread2 = CreateThread(NULL, 0, SendThread, sess, 0, &sess->thread2_id);

            char logmsg[256];
            sprintf(logmsg, "Session created (threads: %d, %d)", sess->thread1_id, sess->thread2_id);
            WriteLog(logmsg);

            EnterCriticalSection(&cs);
            clients.push_back(sess);
            LeaveCriticalSection(&cs);
        }
    }

    InterlockedExchange(&run_flag, 0);

    if (monitor_thread) {
        WaitForSingleObject(monitor_thread, 5000);
        CloseHandle(monitor_thread);
        WriteLog("Monitor thread stopped");
    }

    if (listen_s != INVALID_SOCKET) {
        shutdown(listen_s, SD_BOTH);
        closesocket(listen_s);
        listen_s = INVALID_SOCKET;
    }

    EnterCriticalSection(&cs);
    for (size_t i = 0; i < clients.size(); i++) {
        if (clients[i]) {
            InterlockedExchange(&clients[i]->active, 0);
            if (clients[i]->sock != INVALID_SOCKET) {
                shutdown(clients[i]->sock, SD_BOTH);
                closesocket(clients[i]->sock);
                clients[i]->sock = INVALID_SOCKET;
            }
        }
    }
    LeaveCriticalSection(&cs);

    EnterCriticalSection(&cs);
    for (size_t i = 0; i < clients.size(); i++) {
        CleanupSession(clients[i]);
    }
    clients.clear();
    LeaveCriticalSection(&cs);

cleanup:
    if (listen_s != INVALID_SOCKET) {
        closesocket(listen_s);
    }

    WSACleanup();
    CloseHandle(stop_evt);
    DeleteCriticalSection(&cs);

    WriteLog("Service stopped");
    SetStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

void InstallSvc() {
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        printf("cant open scm: %d\n", GetLastError());
        return;
    }

    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);

    SC_HANDLE svc = CreateServiceA(scm, "RemoteConsoleSvc", "Remote Console Service",
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        path, NULL, NULL, NULL, NULL, NULL);

    if (!svc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            printf("service already exists\n");
        }
        else {
            printf("error creating service: %d\n", err);
        }
        CloseServiceHandle(scm);
        return;
    }

    printf("service installed\n");
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}

void RemoveSvc() {
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        printf("cant open scm\n");
        return;
    }

    SC_HANDLE svc = OpenServiceA(scm, "RemoteConsoleSvc", SERVICE_STOP | DELETE);
    if (!svc) {
        printf("cant open service: %d\n", GetLastError());
        CloseServiceHandle(scm);
        return;
    }

    SERVICE_STATUS status;
    ControlService(svc, SERVICE_CONTROL_STOP, &status);

    if (DeleteService(svc)) {
        printf("service removed\n");
    }
    else {
        printf("failed to delete: %d\n", GetLastError());
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}

void StartSvc() {
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        printf("cant open scm\n");
        return;
    }

    SC_HANDLE svc = OpenServiceA(scm, "RemoteConsoleSvc", SERVICE_START);
    if (!svc) {
        printf("cant open service: %d\n", GetLastError());
        CloseServiceHandle(scm);
        return;
    }

    if (StartServiceA(svc, 0, NULL)) {
        printf("service started\n");
    }
    else {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_ALREADY_RUNNING) {
            printf("service already running\n");
        }
        else {
            printf("failed to start: %d\n", err);
        }
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}

void StopSvc() {
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        printf("cant open scm\n");
        return;
    }

    SC_HANDLE svc = OpenServiceA(scm, "RemoteConsoleSvc", SERVICE_STOP);
    if (!svc) {
        printf("cant open service: %d\n", GetLastError());
        CloseServiceHandle(scm);
        return;
    }

    SERVICE_STATUS status;
    if (ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
        printf("service stopped\n");
    }
    else {
        printf("failed to stop: %d\n", GetLastError());
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}

int main(int argc, char* argv[]) {
    if (argc > 1) {
        if (strcmp(argv[1], "-install") == 0) {
            InstallSvc();
            return 0;
        }
        else if (strcmp(argv[1], "-uninstall") == 0) {
            RemoveSvc();
            return 0;
        }
        else if (strcmp(argv[1], "-start") == 0) {
            StartSvc();
            return 0;
        }
        else if (strcmp(argv[1], "-stop") == 0) {
            StopSvc();
            return 0;
        }
    }

    SERVICE_TABLE_ENTRYA svc_table[2];
    svc_table[0].lpServiceName = "RemoteConsoleSvc";
    svc_table[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTIONA)SvcMain;
    svc_table[1].lpServiceName = NULL;
    svc_table[1].lpServiceProc = NULL;

    if (!StartServiceCtrlDispatcherA(svc_table)) {
        printf("run with -install first\n");
    }

    return 0;
}