/* t7helper: loads Tekken 7 fighters' movesets through the game's own loader, from inside its Wine prefix (run with
 * Proton next to the running game; tools/characters/ttt2char/tekken7/autodump.py drives it).
 *
 *   t7helper serve <port> <secret>     takes one connection at a time on 127.0.0.1:<port> (Proton's launcher passes no
 *                                      stdin/stdout on); the first line must be the secret, then commands, one per
 *                                      line, each answered on the connection:
 *     dlc <steam_api64.dll path>       the game's DLC as the game asks Steam (SteamAppId=389730, Steam running):
 *                                      "<appid> <owned 0/1> <name>" lines, then "end" ("error ..." without Steam)
 *     attach                           finds the game (TekkenGame-Win64-Shipping.exe) and checks that it is the build
 *                                      whose functions this helper knows (by hashes of their first bytes): "ok", or
 *                                      "error ..."
 *     ready                            whether the game's task manager and file loader exist yet: "1" or "0"
 *     common                           starts loading the animations every fighter shares: "<record hex>"
 *     load <roster id>                 starts loading a fighter's files (moveset, animations, command list):
 *                                      "<record hex> <code>", or "error unknown fighter"
 *     fixup <record hex> <common record hex>
 *                                      once loaded, turns the moveset and the command list from their file form
 *                                      into the form the game uses: "<moveset hex> <command list hex>"
 *     read <address hex> <size>        "data <n>" then n raw bytes of the game's memory (n < size where unreadable)
 *     want <path>                      queues one of the game's files for `fetch` (a path in its paks,
 *                                      "../../../TekkenGame/Content/..."): "ok", or "error ..."
 *     fetch                            reads the queued files as the game reads them (its own file reader, which
 *                                      decodes the files it keeps encrypted): "file <0/1> <n>" then n raw bytes per
 *                                      file, in queue order, then "end"; the queue is emptied
 *     quit                             ends the helper
 * The game's functions are called on a thread of their own (they queue work on the game's own threads, or work on the
 * buffers alone); no address or code comes from the connection.
 *
 * Written for this project (MIT); no third-party code.
 */
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ the game (TEKKEN 7, Steam build 10546199) */

/* The game's functions the helper calls, each checked before any call by a hash (64-bit FNV-1a) of its first bytes,
 * so another build of the game is refused, not crashed. */
struct Function {
    unsigned long long address;
    size_t length;               /* bytes hashed */
    unsigned long long hash;
};
static const struct Function ALLOCATE = {0x1409c9d70ull, 7, 0x285C80FEB4ADCB18ull};
static const struct Function FIGHTER_INFO = {0x140759040ull, 16, 0x31A99D12ED74461Full};
static const struct Function LOAD_FIGHTER = {0x1402c8130ull, 16, 0x60D3EFD802971575ull};
static const struct Function LOAD_COMMON = {0x1402c81d0ull, 16, 0xFE35E9993E0F0DFEull};
static const struct Function FIXUP_MOVESET = {0x140714f40ull, 16, 0x418477E3A11B5BD9ull};
static const struct Function FIXUP_MOVELIST = {0x14075ad40ull, 16, 0xAFAAC7406FA4B2CCull};
static const struct Function LOAD_FILE = {0x1408276c0ull, 16, 0xFE6C37993E3D21F8ull}; /* (TArray<u8>*, path, flags) */
static const struct Function FREE = {0x1407cd730ull, 16, 0x5A43178A645AB271ull};
static const unsigned long long TASK_MANAGER = 0x1434d6c18ull, FILE_LOADER = 0x1434cdfa0ull;
/* A fighter record (0x70 bytes): code @0, moveset @0x40, animations @0x48, command list @0x68 (written last); the
 * common record (0x40 bytes): shared animations @0x38. */
enum { RECORD_SIZE = 0x70, RECORD_MOVESET = 0x40, RECORD_ANIMATIONS = 0x48, RECORD_MOVELIST = 0x68, COMMON_SIZE = 0x40,
       COMMON_ANIMATIONS = 0x38, INFO_CODE = 0x68, MAX_FIGHTER = 255 };

static HANDLE g_game;
static int g_checked;

/* ------------------------------------------------------------ answers */

static SOCKET g_sock = INVALID_SOCKET;
static char g_buf[1 << 16];
static size_t g_len;

static void out_flush(void) {
    size_t sent = 0;
    while (sent < g_len) {
        const int n = send(g_sock, g_buf + sent, (int)(g_len - sent), 0);
        if (n <= 0) break;
        sent += (size_t)n;
    }
    g_len = 0;
}
static void out_write(const void* data, size_t n) {
    const char* p = data;
    while (n) {
        if (g_len == sizeof g_buf) out_flush();
        size_t k = sizeof g_buf - g_len;
        if (k > n) k = n;
        memcpy(g_buf + g_len, p, k);
        g_len += k;
        p += k;
        n -= k;
    }
}
static void out_printf(const char* format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (n > 0) out_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
    out_flush();
}

/* ------------------------------------------------------------ Steam: the DLC the player owns */

static void dlc(const char* dll) {
    HMODULE steam = LoadLibraryA(dll && *dll ? dll : "steam_api64.dll");
    if (!steam) {
        out_printf("error steam_api64.dll not found\n");
        return;
    }
    typedef int (*Init)(void);
    typedef void (*Shutdown)(void);
    typedef void* (*Apps)(void);
    typedef int (*Count)(void*);
    typedef int (*Data)(void*, int, unsigned*, int*, char*, int);
    Init init = (Init)(void*)GetProcAddress(steam, "SteamAPI_Init");
    Shutdown shutdown = (Shutdown)(void*)GetProcAddress(steam, "SteamAPI_Shutdown");
    Apps apps = (Apps)(void*)GetProcAddress(steam, "SteamApps");
    Count count = (Count)(void*)GetProcAddress(steam, "SteamAPI_ISteamApps_GetDLCCount");
    Data data = (Data)(void*)GetProcAddress(steam, "SteamAPI_ISteamApps_BGetDLCDataByIndex");
    if (!init || !shutdown || !apps || !count || !data) {
        out_printf("error steam_api64.dll lacks the calls it needs\n");
        return;
    }
    if (!init()) {
        out_printf("error Steam is not running (or does not know the game)\n");
        return;
    }
    void* a = apps();
    const int n = a ? count(a) : 0;
    for (int i = 0; i < n; ++i) {
        unsigned id = 0;
        int owned = 0; /* a C++ bool: only its first byte is written */
        char name[256] = {0};
        if (data(a, i, &id, &owned, name, sizeof name)) out_printf("%u %d %s\n", id, owned & 0xFF, name);
    }
    out_printf("end\n");
    shutdown();
}

/* ------------------------------------------------------------ the game's process */

static DWORD game_pid(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    memset(&pe, 0, sizeof pe);
    pe.dwSize = sizeof pe;
    DWORD pid = 0;
    if (snap == INVALID_HANDLE_VALUE) return 0;
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe)) {
        if (_stricmp(pe.szExeFile, "TekkenGame-Win64-Shipping.exe") == 0) {
            pid = pe.th32ProcessID;
            break;
        }
    }
    CloseHandle(snap);
    return pid;
}

static int read_game(unsigned long long address, void* out, size_t size) {
    SIZE_T got = 0;
    return g_game && ReadProcessMemory(g_game, (void*)(size_t)address, out, size, &got) && got == size;
}

static unsigned long long read_u64(unsigned long long address) {
    unsigned long long v = 0;
    read_game(address, &v, 8);
    return v;
}

static int matches(const struct Function* f) {
    unsigned char b[16];
    if (f->length > sizeof b || !read_game(f->address, b, f->length)) return 0;
    unsigned long long h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < f->length; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
    return h == f->hash;
}

static void attach(void) {
    const DWORD pid = game_pid();
    if (!pid) {
        out_printf("error Tekken 7 is not running\n");
        return;
    }
    if (g_game) CloseHandle(g_game);
    g_game = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                         PROCESS_VM_READ, FALSE, pid);
    if (!g_game) {
        out_printf("error cannot open the game (%lu)\n", (unsigned long)GetLastError());
        return;
    }
    const struct Function* all[] = {&ALLOCATE, &FIGHTER_INFO, &LOAD_FIGHTER, &LOAD_COMMON, &FIXUP_MOVESET,
                                    &FIXUP_MOVELIST, &LOAD_FILE, &FREE};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i) {
        if (!matches(all[i])) {
            out_printf("error this Tekken 7 build is not the one the helper knows\n");
            return;
        }
    }
    g_checked = 1;
    out_printf("ok\n");
}

/* Calls one of the game's functions (four integer arguments) on a thread of its own: -> rax, or ~0 on failure. The
 * stub: sub rsp,0x28; rcx/rdx/r8/r9 = arguments; call the function; store rax; add rsp,0x28; ret. */
static void put64(unsigned char* code, size_t* n, unsigned char a, unsigned char b, unsigned long long v) {
    code[(*n)++] = a;
    code[(*n)++] = b;
    memcpy(code + *n, &v, 8);
    *n += 8;
}
static unsigned long long call(const struct Function* f, unsigned long long a1, unsigned long long a2,
                               unsigned long long a3) {
    if (!g_checked) return ~0ull;
    unsigned char* stub = VirtualAllocEx(g_game, NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return ~0ull;
    const unsigned long long result = (unsigned long long)(size_t)stub + 0x100;
    unsigned char code[96];
    size_t n = 0;
    code[n++] = 0x48; code[n++] = 0x83; code[n++] = 0xEC; code[n++] = 0x28;
    put64(code, &n, 0x48, 0xB9, a1);         /* mov rcx */
    put64(code, &n, 0x48, 0xBA, a2);         /* mov rdx */
    put64(code, &n, 0x49, 0xB8, a3);         /* mov r8 */
    put64(code, &n, 0x48, 0xB8, f->address); /* mov rax */
    code[n++] = 0xFF; code[n++] = 0xD0;      /* call rax */
    put64(code, &n, 0x48, 0xB9, result);     /* mov rcx, result */
    code[n++] = 0x48; code[n++] = 0x89; code[n++] = 0x01;                     /* mov [rcx], rax */
    code[n++] = 0x48; code[n++] = 0x83; code[n++] = 0xC4; code[n++] = 0x28;   /* add rsp, 0x28 */
    code[n++] = 0x31; code[n++] = 0xC0; code[n++] = 0xC3;                     /* xor eax, eax; ret */
    SIZE_T done;
    unsigned long long value = ~0ull;
    if (WriteProcessMemory(g_game, stub, code, n, &done) && WriteProcessMemory(g_game, (void*)(size_t)result, &value, 8, &done)) {
        FlushInstructionCache(g_game, stub, n);
        HANDLE t = CreateRemoteThread(g_game, NULL, 0, (LPTHREAD_START_ROUTINE)stub, NULL, 0, NULL);
        if (t) {
            if (WaitForSingleObject(t, 30000) == WAIT_OBJECT_0) {
                read_game(result, &value, 8);
                VirtualFreeEx(g_game, stub, 0, MEM_RELEASE);
            }
            CloseHandle(t);
        }
    }
    return value;
}

/* Calls a function once per entry of a table, all on one thread of its own (the game keeps a file reader per
 * thread): entries of 32 bytes {path, TArray<u8> {data, i32 count, i32 capacity}, result}; `load` calls
 * function(&entry.array, entry.path, 0), otherwise function(entry.array.data). -> whether the thread finished. */
enum { ENTRY_SIZE = 32, ENTRY_ARRAY = 8, MAX_FILES = 256, PATH_CHARS = 260 };
static int call_each(const struct Function* f, unsigned long long table, unsigned count, int load) {
    if (!g_checked) return 0;
    unsigned char* stub = VirtualAllocEx(g_game, NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return 0;
    unsigned char code[128];
    size_t n = 0;
    code[n++] = 0x53; code[n++] = 0x56;                                       /* push rbx; push rsi */
    code[n++] = 0x48; code[n++] = 0x83; code[n++] = 0xEC; code[n++] = 0x28;   /* sub rsp, 0x28 */
    put64(code, &n, 0x48, 0xBB, table);                                       /* mov rbx, table */
    code[n++] = 0xBE; memcpy(code + n, &count, 4); n += 4;                    /* mov esi, count */
    const size_t loop = n;
    code[n++] = 0x85; code[n++] = 0xF6;                                       /* test esi, esi */
    code[n++] = 0x74; const size_t done = n++;                                /* je done */
    if (load) {
        code[n++] = 0x48; code[n++] = 0x8D; code[n++] = 0x4B; code[n++] = ENTRY_ARRAY; /* lea rcx, [rbx+8] */
        code[n++] = 0x48; code[n++] = 0x8B; code[n++] = 0x13;                         /* mov rdx, [rbx] */
        code[n++] = 0x45; code[n++] = 0x31; code[n++] = 0xC0;                         /* xor r8d, r8d */
    } else {
        code[n++] = 0x48; code[n++] = 0x8B; code[n++] = 0x4B; code[n++] = ENTRY_ARRAY; /* mov rcx, [rbx+8] */
    }
    put64(code, &n, 0x48, 0xB8, f->address);                                  /* mov rax, function */
    code[n++] = 0xFF; code[n++] = 0xD0;                                       /* call rax */
    code[n++] = 0x48; code[n++] = 0x89; code[n++] = 0x43; code[n++] = 0x18;   /* mov [rbx+0x18], rax */
    code[n++] = 0x48; code[n++] = 0x83; code[n++] = 0xC3; code[n++] = ENTRY_SIZE; /* add rbx, 32 */
    code[n++] = 0xFF; code[n++] = 0xCE;                                       /* dec esi */
    code[n++] = 0xEB; code[n] = (unsigned char)(loop - (n + 1)); n++;         /* jmp loop */
    code[done] = (unsigned char)(n - (done + 1));
    code[n++] = 0x48; code[n++] = 0x83; code[n++] = 0xC4; code[n++] = 0x28;   /* add rsp, 0x28 */
    code[n++] = 0x5E; code[n++] = 0x5B;                                       /* pop rsi; pop rbx */
    code[n++] = 0x31; code[n++] = 0xC0; code[n++] = 0xC3;                     /* xor eax, eax; ret */
    SIZE_T written;
    int finished = 0;
    if (WriteProcessMemory(g_game, stub, code, n, &written)) {
        FlushInstructionCache(g_game, stub, n);
        HANDLE t = CreateRemoteThread(g_game, NULL, 0, (LPTHREAD_START_ROUTINE)stub, NULL, 0, NULL);
        if (t) {
            finished = WaitForSingleObject(t, 600000) == WAIT_OBJECT_0;
            CloseHandle(t);
        }
    }
    if (finished) VirtualFreeEx(g_game, stub, 0, MEM_RELEASE);
    return finished;
}

static char g_wanted[MAX_FILES][PATH_CHARS];
static unsigned g_wanted_count;

/* Only the game's own content, by a plain relative path: "../../../TekkenGame/Content/" then no "..", ':' or '\'. */
static void want(const char* path) {
    static const char prefix[] = "../../../TekkenGame/Content/";
    const size_t len = strlen(path);
    if (strncmp(path, prefix, sizeof prefix - 1) || len >= PATH_CHARS || strstr(path + sizeof prefix - 1, "..") ||
        strpbrk(path, ":\\")) {
        out_printf("error not a path of the game's content\n");
        return;
    }
    if (g_wanted_count == MAX_FILES) {
        out_printf("error too many files queued\n");
        return;
    }
    memcpy(g_wanted[g_wanted_count++], path, len + 1);
    out_printf("ok\n");
}

static void fetch(void) {
    const unsigned count = g_wanted_count;
    g_wanted_count = 0;
    const size_t table_size = (size_t)count * ENTRY_SIZE, size = table_size + (size_t)count * PATH_CHARS * 2;
    unsigned char* local = calloc(1, size ? size : 1);
    unsigned char* remote = count ? VirtualAllocEx(g_game, NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : NULL;
    SIZE_T done;
    if (!g_checked || !local || (count && !remote)) {
        out_printf("error cannot allocate\n");
        free(local);
        return;
    }
    for (unsigned i = 0; i < count; ++i) {   /* entries, then the paths (UTF-16) */
        const size_t at = table_size + (size_t)i * PATH_CHARS * 2;
        const unsigned long long path = (unsigned long long)(size_t)remote + at;
        memcpy(local + (size_t)i * ENTRY_SIZE, &path, 8);
        for (size_t k = 0; g_wanted[i][k]; ++k) local[at + 2 * k] = (unsigned char)g_wanted[i][k];
    }
    if (count && (!WriteProcessMemory(g_game, remote, local, size, &done) ||
                  !call_each(&LOAD_FILE, (unsigned long long)(size_t)remote, count, 1) ||
                  !read_game((unsigned long long)(size_t)remote, local, table_size))) {
        out_printf("error the game did not read the files\n");   /* (memory left as it is: a thread may still run) */
        free(local);
        return;
    }
    for (unsigned i = 0; i < count; ++i) {
        const unsigned char* e = local + (size_t)i * ENTRY_SIZE;
        unsigned long long data, result;
        int length;
        memcpy(&data, e + ENTRY_ARRAY, 8);
        memcpy(&length, e + ENTRY_ARRAY + 8, 4);
        memcpy(&result, e + 24, 8);
        char* bytes = length > 0 && data ? malloc((size_t)length) : NULL;
        const int ok = (result & 0xFF) && bytes && read_game(data, bytes, (size_t)length);
        out_printf("file %d %d\n", ok, ok ? length : 0);
        if (ok) out_write(bytes, (size_t)length);
        out_flush();
        free(bytes);
    }
    if (count) {
        call_each(&FREE, (unsigned long long)(size_t)remote, count, 0);
        VirtualFreeEx(g_game, remote, 0, MEM_RELEASE);
    }
    out_printf("end\n");
    free(local);
}

static void ready(void) {
    out_printf("%d\n", g_checked && read_u64(TASK_MANAGER) && read_u64(FILE_LOADER) ? 1 : 0);
}

static void common(void) {
    const unsigned long long record = call(&ALLOCATE, COMMON_SIZE, 0, 0);
    if (record == ~0ull || !record) {
        out_printf("error cannot allocate\n");
        return;
    }
    call(&LOAD_COMMON, record, 0, 0);
    out_printf("%llx\n", record);
}

static void load(unsigned id) {
    if (id > MAX_FIGHTER) {
        out_printf("error unknown fighter\n");
        return;
    }
    const unsigned long long info = call(&FIGHTER_INFO, id, 0, 0);
    const unsigned long long code = info != ~0ull ? read_u64(info + INFO_CODE) : 0;
    char name[4] = {0};
    if (!code || !read_game(code, name, 3) || !name[0]) {
        out_printf("error unknown fighter\n");
        return;
    }
    const unsigned long long record = call(&ALLOCATE, RECORD_SIZE, 0, 0);
    if (record == ~0ull || !record) {
        out_printf("error cannot allocate\n");
        return;
    }
    call(&LOAD_FIGHTER, record, code, 0);
    out_printf("%llx %s\n", record, name);
}

static void fixup(unsigned long long record, unsigned long long common_record) {
    const unsigned long long moveset = read_u64(record + RECORD_MOVESET);
    const unsigned long long animations = read_u64(record + RECORD_ANIMATIONS);
    const unsigned long long movelist = read_u64(record + RECORD_MOVELIST);
    const unsigned long long shared = read_u64(common_record + COMMON_ANIMATIONS);
    if (!moveset || !animations || !movelist || !shared) {
        out_printf("error not loaded\n");
        return;
    }
    call(&FIXUP_MOVESET, moveset, animations, shared);
    call(&FIXUP_MOVELIST, movelist, 0, 0);
    out_printf("%llx %llx\n", moveset, movelist);
}

static void read_memory(unsigned long long address, size_t size) {
    char* buffer = malloc(size ? size : 1);
    size_t got = 0;
    while (buffer && g_game && got < size) {          /* page by page past what cannot be read */
        SIZE_T n = 0, want = 4096 - ((address + got) & 4095);
        if (want > size - got) want = size - got;
        if (!ReadProcessMemory(g_game, (void*)(size_t)(address + got), buffer + got, want, &n) || n == 0) break;
        got += n;
    }
    out_printf("data %llu\n", (unsigned long long)got);
    if (got) out_write(buffer, got);
    out_flush();
    free(buffer);
}

/* ------------------------------------------------------------ the connection */

static int serve(int port, const char* secret) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 2;
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in at;
    memset(&at, 0, sizeof at);
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    at.sin_port = htons((u_short)port);
    if (listener == INVALID_SOCKET || bind(listener, (struct sockaddr*)&at, sizeof at) != 0 || listen(listener, 1) != 0) {
        fprintf(stderr, "t7helper: cannot listen on 127.0.0.1:%d\n", port);
        return 2;
    }
    char input[4096], line[512];
    size_t have = 0;
    int quit = 0, trusted = 0;
    while (!quit) {
        if (g_sock == INVALID_SOCKET) { /* the next client */
            g_sock = accept(listener, NULL, NULL);
            have = 0;
            trusted = 0;
            if (g_sock == INVALID_SOCKET) break;
        }
        char* end = memchr(input, '\n', have);
        if (!end) {
            if (have == sizeof input) have = 0; /* an overlong line: dropped */
            const int n = recv(g_sock, input + have, (int)(sizeof input - have), 0);
            if (n <= 0) {
                closesocket(g_sock);
                g_sock = INVALID_SOCKET;
                continue;
            }
            have += (size_t)n;
            continue;
        }
        size_t len = (size_t)(end - input);
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, input, len);
        line[len] = 0;
        if (len && line[len - 1] == '\r') line[len - 1] = 0;
        have -= (size_t)(end - input) + 1;
        memmove(input, end + 1, have);
        if (!trusted) {                    /* the first line: the secret, or the connection is dropped */
            trusted = strcmp(line, secret) == 0;
            if (!trusted) {
                closesocket(g_sock);
                g_sock = INVALID_SOCKET;
            }
            continue;
        }
        char cmd[32] = {0}, arg[300] = {0};
        unsigned long long a = 0, b = 0;
        if (sscanf(line, "%31s", cmd) != 1) continue;
        if (!strcmp(cmd, "quit")) {
            quit = 1;
        } else if (!strcmp(cmd, "dlc") && sscanf(line, "%*s %299[^\n]", arg) == 1) {
            dlc(arg);
        } else if (!strcmp(cmd, "attach")) {
            attach();
        } else if (!strcmp(cmd, "ready")) {
            ready();
        } else if (!strcmp(cmd, "common")) {
            common();
        } else if (!strcmp(cmd, "load") && sscanf(line, "%*s %llu", &a) == 1) {
            load((unsigned)a);
        } else if (!strcmp(cmd, "fixup") && sscanf(line, "%*s %llx %llx", &a, &b) == 2) {
            fixup(a, b);
        } else if (!strcmp(cmd, "read") && sscanf(line, "%*s %llx %llu", &a, &b) == 2 && b <= (256ull << 20)) {
            read_memory(a, (size_t)b);
        } else if (!strcmp(cmd, "want") && sscanf(line, "%*s %299[^\n]", arg) == 1) {
            want(arg);
        } else if (!strcmp(cmd, "fetch")) {
            fetch();
        } else {
            out_printf("error %s\n", line);
        }
    }
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    closesocket(listener);
    WSACleanup();
    if (g_game) CloseHandle(g_game);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 4 && !strcmp(argv[1], "serve")) return serve(atoi(argv[2]), argv[3]);
    fprintf(stderr, "usage: t7helper serve <port> <secret>\n");
    return 1;
}
