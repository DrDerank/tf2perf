// tf2perf_cli.c - control CLI for tf2perf.dll (attaches to TF2 over \\.\pipe\tf2perf)
//
// Usage:
//   tf2perf                 interactive REPL (attach to a running injected game)
//   tf2perf inject          find TF2, inject tf2perf.dll, then REPL
//   tf2perf <command> ...   send one command and print the reply
//
// Build: build.cmd

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

#define PIPE_NAME "\\\\.\\pipe\\tf2perf"
#define END_MARK  "###END###"

static int connect_pipe(int wait_seconds)
{
    for (int i = 0; i < wait_seconds * 10; i++)
    {
        HANDLE h = CreateFileA(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE)
            return (int)(intptr_t)h;
        if (WaitNamedPipeA(PIPE_NAME, 100))
        {
            h = CreateFileA(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                            OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE)
                return (int)(intptr_t)h;
        }
        Sleep(100);
    }
    return -1;
}

static int send_command(int pipe, const char* cmd)
{
    HANDLE h = (HANDLE)(intptr_t)pipe;
    DWORD written = 0;
    if (!WriteFile(h, cmd, (DWORD)strlen(cmd), &written, NULL))
        return 0;

    char buf[8192];
    for (;;)
    {
        DWORD read = 0;
        if (!ReadFile(h, buf, sizeof(buf) - 1, &read, NULL) || read == 0)
            return 0;
        buf[read] = 0;

        char* end = strstr(buf, END_MARK);
        if (end) *end = 0;
        fputs(buf, stdout);
        if (end) break;
    }
    fflush(stdout);
    return 1;
}

// ---------------------------------------------------------------------------
// injection
// ---------------------------------------------------------------------------
static const char* g_candidates[] = { "hl2.exe", "tf_win64.exe", "tf.exe", "hl2_win64.exe" };

static int process_has_client_dll(DWORD pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    int found = 0;
    if (Module32First(snap, &me))
    {
        do {
            if (!_stricmp(me.szModule, "client.dll"))
            {
                found = 1;
                break;
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

static DWORD find_tf2_pid(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32First(snap, &pe))
    {
        do {
            for (int i = 0; i < (int)(sizeof(g_candidates) / sizeof(g_candidates[0])); i++)
            {
                if (!_stricmp(pe.szExeFile, g_candidates[i]) && process_has_client_dll(pe.th32ProcessID))
                {
                    pid = pe.th32ProcessID;
                    printf("[+] found %s pid=%lu with client.dll loaded\n", pe.szExeFile, pid);
                    break;
                }
            }
            if (pid) break;
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static int inject(DWORD pid, const char* dll_path)
{
    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc) { printf("[-] OpenProcess failed: %lu\n", GetLastError()); return 0; }

    SIZE_T len = strlen(dll_path) + 1;
    LPVOID remote = VirtualAllocEx(hProc, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { printf("[-] VirtualAllocEx failed: %lu\n", GetLastError()); CloseHandle(hProc); return 0; }

    if (!WriteProcessMemory(hProc, remote, dll_path, len, NULL))
    { printf("[-] WriteProcessMemory failed: %lu\n", GetLastError()); CloseHandle(hProc); return 0; }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FARPROC loadlib = GetProcAddress(k32, "LoadLibraryA");
    if (!loadlib) { printf("[-] LoadLibraryA not found\n"); CloseHandle(hProc); return 0; }

    HANDLE th = CreateRemoteThread(hProc, NULL, 0, (LPTHREAD_START_ROUTINE)loadlib, remote, 0, NULL);
    if (!th) { printf("[-] CreateRemoteThread failed: %lu\n", GetLastError()); CloseHandle(hProc); return 0; }

    WaitForSingleObject(th, 10000);
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    CloseHandle(th);
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hProc);

    if (!code) { printf("[-] LoadLibrary returned NULL in target (DLL blocked?)\n"); return 0; }
    printf("[+] injected tf2perf.dll (module=0x%08lX)\n", code);
    return 1;
}

static void dll_path_beside_exe(const char* argv0, char* out, size_t cap)
{
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (!n || n >= sizeof(exe)) { strncpy(out, "tf2perf.dll", cap - 1); return; }
    char* slash = strrchr(exe, '\\');
    if (slash) *(slash + 1) = 0;
    snprintf(out, cap, "%stf2perf.dll", exe);
    (void)argv0;
}

// ---------------------------------------------------------------------------
// REPL
// ---------------------------------------------------------------------------
static void repl(int pipe)
{
    char line[512];
    printf("tf2perf attached. type 'help' for commands, 'quit' to exit.\n");
    for (;;)
    {
        printf("tf2perf> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;

        size_t l = strlen(line);
        while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
        if (!l) continue;
        if (!_stricmp(line, "quit") || !_stricmp(line, "exit")) break;
        if (!_stricmp(line, "clear")) { system("cls"); continue; }

        if (!send_command(pipe, line))
        {
            printf("[-] pipe error, game may have closed\n");
            break;
        }
    }
}

int main(int argc, char** argv)
{
    if (argc >= 2 && (!_stricmp(argv[1], "-h") || !_stricmp(argv[1], "--help")))
    {
        printf("usage:\n"
               "  tf2perf                 attach to a running injected game (REPL)\n"
               "  tf2perf inject          find TF2, inject tf2perf.dll, then REPL\n"
               "  tf2perf <command> ...   send one command\n");
        return 0;
    }

    if (argc >= 2 && !_stricmp(argv[1], "inject"))
    {
        char dll[MAX_PATH];
        dll_path_beside_exe(argv[0], dll, sizeof(dll));
        if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES)
        {
            printf("[-] %s not found next to this exe\n", dll);
            return 1;
        }

        int pipe = connect_pipe(1);
        if (pipe >= 0) { printf("[=] already attached (tf2perf.dll is loaded)\n"); CloseHandle((HANDLE)(intptr_t)pipe); }

        DWORD pid = find_tf2_pid();
        if (!pid)
        {
            printf("[-] TF2 process with client.dll not found (is the game running?)\n");
            return 1;
        }
        if (!inject(pid, dll)) return 1;

        pipe = connect_pipe(15);
        if (pipe < 0) { printf("[-] injected but pipe never appeared, check %%TEMP%%\\tf2perf.log\n"); return 1; }
        repl(pipe);
        CloseHandle((HANDLE)(intptr_t)pipe);
        return 0;
    }

    // one-shot or REPL
    int pipe = connect_pipe(1);
    if (pipe < 0)
    {
        printf("[-] not attached (is TF2 running with tf2perf.dll injected? try: tf2perf inject)\n");
        return 1;
    }

    if (argc >= 2)
    {
        char cmd[1024] = {0};
        for (int i = 1; i < argc; i++)
        {
            strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
            if (i + 1 < argc) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        }
        int ok = send_command(pipe, cmd);
        CloseHandle((HANDLE)(intptr_t)pipe);
        return ok ? 0 : 1;
    }

    repl(pipe);
    CloseHandle((HANDLE)(intptr_t)pipe);
    return 0;
}
