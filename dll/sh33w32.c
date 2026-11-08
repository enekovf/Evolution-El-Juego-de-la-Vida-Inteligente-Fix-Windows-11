/* Minimal replacement for SmartHeap's SH33W32.DLL, backed by a Win32 heap.
   Only implements the 14 ordinals EVOLUTION.EXE imports. */
#define COBJMACROS
#include <windows.h>
#include <tlhelp32.h>
#include <mfplay.h>

/* Set while a remastered MP4 replaces the current Smacker movie: the game's
   player then runs one silent, invisible frame and stops (see InstallHdMovies). */
static volatile int g_hdPlayed;

#define MEM_ZEROINIT_FLAG 0x0001u

static HANDLE g_heap;
static CRITICAL_SECTION g_cs;   /* the game's lock (_shi_enterCriticalSection) */
/* The free quarantine has its own lock: sharing g_cs deadlocked threads that
   wait on each other while holding the game's lock. */
static CRITICAL_SECTION g_qcs;

/* --- Movie player fixes for EVOLUTION.EXE ---------------------------------
   The player switched the screen to 640x480x16, blitted every frame to the
   DirectDraw primary (which stays black on current Windows) and spun on
   SmackWait with no sleep. Now the display mode is left alone, frames go to
   the player window through GDI and the wait sleeps. */
#define SMACKWAIT_IAT 0x0050B694u
#define VT(obj, idx) ((*(void***)(obj))[idx])

typedef LONG (__stdcall *CreateSurfaceFn)(void*, DWORD*, void**, void*);
typedef LONG (__stdcall *GetDCFn)(void*, HDC*);
typedef LONG (__stdcall *ReleaseDCFn)(void*, HDC);
typedef int  (__stdcall *SmackWaitFn)(void*);

static LONG __stdcall VideoPresent(BYTE* player, DWORD x, DWORD y, RECT* src)
{
    static HWND painted;
    void* offscreen;
    if (g_hdPlayed) return 0;
    offscreen = *(void**)(player + 0x44);
    HWND wnd = FindWindowA(NULL, "SmackPlayer");
    HDC surfaceDc = NULL;
    if (wnd && ((GetDCFn)VT(offscreen, 17))(offscreen, &surfaceDc) == 0) {
        HDC wndDc = GetDC(wnd);
        if (wndDc) {
            /* The game centers the movie in a 640x480 frame, so its size follows
               from the blit offset. That frame is fitted 4:3 into the window. */
            int w = 640 - 2 * ((int)x - src->left);
            int h = 480 - 2 * ((int)y - src->top);
            int k = (w * 2 <= 640 && h * 2 <= 480) ? 2 : 1;
            int fx = (640 - w * k) / 2 + src->left * k;
            int fy = (480 - h * k) / 2 + src->top * k;
            int fr = fx + (src->right - src->left) * k;
            int fb = fy + (src->bottom - src->top) * k;
            RECT c;
            int aw, ah, ox, oy;
            GetClientRect(wnd, &c);
            ah = c.bottom;
            aw = MulDiv(ah, 4, 3);
            if (aw > c.right) { aw = c.right; ah = MulDiv(aw, 3, 4); }
            ox = (c.right - aw) / 2;
            oy = (c.bottom - ah) / 2;
            if (painted != wnd) {
                FillRect(wndDc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
                painted = wnd;
            }
            {
                /* The surface keeps the whole frame, so the whole frame is drawn
                   every time: filtering only the changed rectangle would leave
                   seams at its edges. HALFTONE smooths the enlargement. */
                int x0 = (640 - w * k) / 2, y0 = (480 - h * k) / 2;
                int dl = ox + MulDiv(x0, aw, 640), dt = oy + MulDiv(y0, ah, 480);
                int dr = ox + MulDiv(x0 + w * k, aw, 640), db = oy + MulDiv(y0 + h * k, ah, 480);
                (void)fr; (void)fb;
                SetStretchBltMode(wndDc, HALFTONE);
                SetBrushOrgEx(wndDc, 0, 0, NULL);
                StretchBlt(wndDc, dl, dt, dr - dl, db - dt, surfaceDc, 0, 0, w, h, SRCCOPY);
            }
            ReleaseDC(wnd, wndDc);
        }
        ((ReleaseDCFn)VT(offscreen, 26))(offscreen, surfaceDc);
    }
    return 0;
}

/* The offscreen surface must stay RGB565 for Smacker now that the display
   mode is not changed to 16 bits. */
static LONG __stdcall CreateMovieSurface(void* dd, DWORD* desc, void** out, void* outer)
{
    desc[1] |= 0x1000;      /* DDSD_PIXELFORMAT */
    desc[18] = 0x20;        /* ddpf.dwSize */
    desc[19] = 0x40;        /* DDPF_RGB */
    desc[20] = 0;
    desc[21] = 16;
    desc[22] = 0xF800;
    desc[23] = 0x07E0;
    desc[24] = 0x001F;
    desc[25] = 0;
    return ((CreateSurfaceFn)VT(dd, 6))(dd, desc, out, outer);
}

/* SmackWait can keep saying "not yet" forever when the movie clock (tied to
   its sound) stalls, which froze the game in some disaster movies. After
   250 ms of waiting the next frame is shown anyway. Only the main thread
   plays movies, so plain statics are enough. */
static int __stdcall WaitForFrame(void* smack)
{
    static DWORD waitingSince;
    int r;
    if (g_hdPlayed) return 0;
    r = (*(SmackWaitFn*)SMACKWAIT_IAT)(smack);
    if (!r) {
        waitingSince = 0;
        return 0;
    }
    if (!waitingSince) {
        waitingSince = GetTickCount();
    } else if (GetTickCount() - waitingSince > 250) {
        waitingSince = 0;
        return 0;
    }
    Sleep(1);
    return r;
}

static void PatchBytes(BYTE* at, const BYTE* expected, SIZE_T expectedLen, const BYTE* code, SIZE_T len)
{
    DWORD old;
    SIZE_T i;
    for (i = 0; i < expectedLen; i++)
        if (at[i] != expected[i]) return;
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old)) return;
    for (i = 0; i < len; i++) at[i] = code[i];
    VirtualProtect(at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, len);
}

static void InstallMovieFixes(void)
{
    /* 00450A1C: push 10h / lea eax,[ebp-34h] ... call [edi+1Ch] / jmp 00450A4F */
    static const BYTE bltExpected[] = { 0x6A, 0x10, 0x8D, 0x45, 0xCC, 0x50 };
    /* lea eax,[ebp-34h]; push eax; push [ebp-20h]; push [ebp-24h]; push esi;
       call VideoPresent; jmp 00450A4F */
    BYTE blt[18] = { 0x8D, 0x45, 0xCC, 0x50, 0xFF, 0x75, 0xE0, 0xFF, 0x75, 0xDC, 0x56,
                     0xE8, 0, 0, 0, 0, 0xEB, 0x21 };
    /* 00450877: call dword ptr ds:[0050B694h] */
    static const BYTE waitExpected[] = { 0xFF, 0x15, 0x94, 0xB6, 0x50, 0x00 };
    BYTE wait[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
    DWORD rel;

    rel = (DWORD)(ULONG_PTR)VideoPresent - (0x00450A1Cu + 16u);
    CopyMemory(blt + 12, &rel, 4);
    PatchBytes((BYTE*)0x00450A1C, bltExpected, sizeof bltExpected, blt, sizeof blt);

    rel = (DWORD)(ULONG_PTR)WaitForFrame - (0x00450877u + 5u);
    CopyMemory(wait + 1, &rel, 4);
    PatchBytes((BYTE*)0x00450877, waitExpected, sizeof waitExpected, wait, sizeof wait);

    /* 00450422: SetCooperativeLevel FULLSCREEN|EXCLUSIVE -> NORMAL */
    {
        static const BYTE e[] = { 0x6A, 0x11 };
        static const BYTE c[] = { 0x6A, 0x08 };
        PatchBytes((BYTE*)0x00450422, e, sizeof e, c, sizeof c);
    }
    /* 004504B4: call SetDisplayMode; test eax,eax -> add esp,10h; xor eax,eax */
    {
        static const BYTE e[] = { 0xFF, 0x57, 0x54, 0x85, 0xC0 };
        static const BYTE c[] = { 0x83, 0xC4, 0x10, 0x33, 0xC0 };
        PatchBytes((BYTE*)0x004504B4, e, sizeof e, c, sizeof c);
    }
    /* 004505AA: offscreen CreateSurface -> CreateMovieSurface (test eax follows) */
    {
        static const BYTE e[] = { 0xFF, 0x50, 0x18, 0x89, 0x45, 0xE4 };
        BYTE c[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
        rel = (DWORD)(ULONG_PTR)CreateMovieSurface - (0x004505AAu + 5u);
        CopyMemory(c + 1, &rel, 4);
        PatchBytes((BYTE*)0x004505AA, e, sizeof e, c, sizeof c);
    }
    /* 004506DA: read the pixel format from the offscreen surface, not the primary */
    {
        static const BYTE e[] = { 0x8B, 0x46, 0x40 };
        static const BYTE c[] = { 0x8B, 0x46, 0x44 };
        PatchBytes((BYTE*)0x004506DA, e, sizeof e, c, sizeof c);
    }
}

/* --- Play without the CD --------------------------------------------------
   The game only needs the disc for movies and music, and the local install
   folder holds copies of both. The game's own "-nocdkjm" mode skips the disc
   check. Paths stay short and relative (the player and Smacker keep them in
   fixed-size buffers: long absolute paths corrupted memory), and the current
   directory is set to the game folder right before each one is built. */
static char g_gameDir[MAX_PATH];
static const char g_introRel[] = "Movies\\intro_30.smk";
static const char g_moviesRel[] = "Movies\\";
#define MUSIC_REL_FMT 0x00503BD4  /* the game's own "Music\Track %d.wav" */

static void __stdcall EnterGameDir(void)
{
    if (g_gameDir[0]) SetCurrentDirectoryA(g_gameDir);
}

/* Each stub replaces a 5-byte "push imm32": it sets the current directory,
   then leaves the string pointer on the stack exactly as the push did. */
#define PUSH_STUB(name, value)                                     \
    static __declspec(naked) void name(void)                       \
    {                                                              \
        __asm pushad                                               \
        __asm call EnterGameDir                                    \
        __asm popad                                                \
        __asm push eax                                             \
        __asm mov eax, dword ptr [esp + 4]                         \
        __asm mov dword ptr [esp + 4], value                       \
        __asm xchg eax, dword ptr [esp]                            \
        __asm ret                                                  \
    }
PUSH_STUB(PushIntroPath, offset g_introRel)
PUSH_STUB(PushMoviesDir, offset g_moviesRel)
PUSH_STUB(PushMusicFmt, MUSIC_REL_FMT)

static void PatchPushToCall(DWORD at, DWORD oldImm, void (*stub)(void))
{
    BYTE e[5], c[5];
    DWORD rel = (DWORD)(ULONG_PTR)stub - (at + 5u);
    e[0] = 0x68;  /* push imm32 */
    CopyMemory(e + 1, &oldImm, 4);
    c[0] = 0xE8;  /* call rel32 */
    CopyMemory(c + 1, &rel, 4);
    PatchBytes((BYTE*)(ULONG_PTR)at, e, 5, c, 5);
}

static void InstallNoCdFixes(void)
{
    DWORD n = GetModuleFileNameA(NULL, g_gameDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { g_gameDir[0] = 0; return; }
    while (n > 0 && g_gameDir[n - 1] != '\\') n--;
    g_gameDir[n] = 0;
    EnterGameDir();

    /* 00438CEE: mov [ebp-48h],1 (disc required) -> 0, as with -nocdkjm */
    {
        static const BYTE e[] = { 0xC7, 0x45, 0xB8, 0x01, 0x00, 0x00, 0x00 };
        static const BYTE c[] = { 0xC7, 0x45, 0xB8, 0x00, 0x00, 0x00, 0x00 };
        PatchBytes((BYTE*)0x00438CEE, e, sizeof e, c, sizeof c);
    }
    /* Movie paths: "%c:\Evolution\Movies\intro_30.smk" and "%c:\Evolution\movies\" */
    PatchPushToCall(0x0044FEEC, 0x0050266C, PushIntroPath);
    PatchPushToCall(0x004501E0, 0x005026E0, PushMoviesDir);
#ifndef NO_MUSIC_PATCH
    /* Music: always take the game's own "no CD" branch (relative path) */
    {
        static const BYTE j1[] = { 0x75, 0x19 }, j2[] = { 0x75, 0x13 };
        static const BYTE nop[] = { 0x90, 0x90 };
        PatchBytes((BYTE*)0x00464A74, j1, 2, nop, 2);
        PatchBytes((BYTE*)0x00465A92, j2, 2, nop, 2);
        PatchBytes((BYTE*)0x00465C2C, j2, 2, nop, 2);
    }
    PatchPushToCall(0x00464A7A, MUSIC_REL_FMT, PushMusicFmt);
    PatchPushToCall(0x00465A94, MUSIC_REL_FMT, PushMusicFmt);
    PatchPushToCall(0x00465C2E, MUSIC_REL_FMT, PushMusicFmt);
#endif
}

/* --- Remastered movies -------------------------------------------------------
   If MOVIES\HD\<NAME>.mp4 exists for a movie (NAME without the _30/_15
   suffix, e.g. MOVIES\HD\INTRO.mp4 for Movies\intro_30.smk), it is played
   with Media Foundation (MFPlay) in the player window instead of the Smacker
   file. The game's player code still runs afterwards so that it sets up and
   cleans up as usual: the Smacker movie is opened, but its only frame is not
   drawn, its sound is switched off and the player's Esc check reports a key
   press, which ends it at once. Without the MP4 everything works as before. */
#define SMACKOPEN_IAT 0x0050B67Cu
typedef void* (__stdcall* SmackOpenFn)(const char*, DWORD, DWORD);
typedef void (__stdcall* SmackSoundOnOffFn)(void*, DWORD);
typedef HRESULT (STDAPICALLTYPE* MfpCreateFn)(LPCWSTR, BOOL, MFP_CREATION_OPTIONS,
                                              IMFPMediaPlayerCallback*, HWND, IMFPMediaPlayer**);

static BOOL HdMoviePath(const char* smk, WCHAR* out, int outLen)
{
    const char* base = smk;
    const char* p;
    char name[64], path[MAX_PATH + 96];
    int n = 0;
    for (p = smk; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
    while (base[n] && base[n] != '.' && n < 60) { name[n] = base[n]; n++; }
    name[n] = 0;
    if (n > 3 && name[n - 3] == '_' && name[n - 2] >= '0' && name[n - 2] <= '9' &&
        name[n - 1] >= '0' && name[n - 1] <= '9')
        name[n - 3] = 0;
    if (!name[0] || !g_gameDir[0]) return FALSE;
    wsprintfA(path, "%sMOVIES\\HD\\%s.mp4", g_gameDir, name);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return FALSE;
    return MultiByteToWideChar(CP_ACP, 0, path, -1, out, outLen) > 0;
}

/* Plays the file in the player window and returns once it ends or Esc is
   pressed. Returns FALSE if it could not be played at all. */
static BOOL PlayHdMovie(const WCHAR* url)
{
    HWND wnd = FindWindowA(NULL, "SmackPlayer");
    HMODULE mfplay;
    MfpCreateFn create;
    IMFPMediaPlayer* player = NULL;
    HRESULT co;
    BOOL started = FALSE;
    if (!wnd) return FALSE;
    mfplay = LoadLibraryA("mfplay.dll");
    create = mfplay ? (MfpCreateFn)GetProcAddress(mfplay, "MFPCreateMediaPlayer") : NULL;
    if (!create) return FALSE;
    co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (!IsWindowVisible(wnd)) ShowWindow(wnd, SW_SHOW);
    {
        RECT c;
        HDC dc = GetDC(wnd);
        if (dc) {
            GetClientRect(wnd, &c);
            FillRect(dc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
            ReleaseDC(wnd, dc);
        }
    }
    if (SUCCEEDED(create(url, FALSE, 0, NULL, wnd, &player)) && player) {
        IMFPMediaPlayer_SetBorderColor(player, RGB(0, 0, 0));
        if (SUCCEEDED(IMFPMediaPlayer_Play(player))) {
            DWORD t0 = GetTickCount();
            for (;;) {
                MSG msg;
                MFP_MEDIAPLAYER_STATE st;
                BOOL quit = FALSE;
                /* MFPlay needs this thread's messages; the game's own input and
                   timers are left out so it does not run while the movie plays. */
                while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); quit = TRUE; break; }
                    if (msg.message == WM_TIMER ||
                        (msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST) ||
                        (msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST))
                        continue;
                    TranslateMessage(&msg);
                    DispatchMessageA(&msg);
                }
                if (quit) break;
                if (FAILED(IMFPMediaPlayer_GetState(player, &st))) break;
                if (st == MFP_MEDIAPLAYER_STATE_PLAYING) started = TRUE;
                else if (started) break;                      /* finished */
                else if (GetTickCount() - t0 > 8000) break;   /* never started */
                if (started && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) break;
                Sleep(10);
            }
        }
        IMFPMediaPlayer_Shutdown(player);
        IMFPMediaPlayer_Release(player);
    }
    if (co == S_OK || co == S_FALSE) CoUninitialize();
    return started;
}

static void* __stdcall SmackOpenHd(const char* name, DWORD flags, DWORD extra)
{
    WCHAR url[MAX_PATH + 96];
    void* h;
    BOOL hd = FALSE;
    g_hdPlayed = 0;
    if (name && HdMoviePath(name, url, sizeof url / sizeof url[0]))
        hd = PlayHdMovie(url);
    h = (*(SmackOpenFn*)SMACKOPEN_IAT)(name, flags, extra);
    if (h && hd) {
        SmackSoundOnOffFn sound = (SmackSoundOnOffFn)GetProcAddress(GetModuleHandleA("smackw32.dll"),
                                                                    "_SmackSoundOnOff@8");
        if (sound) sound(h, 0);
        g_hdPlayed = 1;
    }
    return h;
}

/* Replaces the player's GetAsyncKeyState(VK_ESCAPE): after an MP4 it ends the
   Smacker movie right after its first (hidden) frame. */
static SHORT __stdcall MovieEscCheck(int vk)
{
    if (g_hdPlayed) {
        g_hdPlayed = 0;
        return (SHORT)0x8000;
    }
    return GetAsyncKeyState(vk);
}

static void PatchCall6(DWORD at, DWORD iat, void* target)
{
    BYTE e[6] = { 0xFF, 0x15 }, c[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
    DWORD rel = (DWORD)(ULONG_PTR)target - (at + 5u);
    CopyMemory(e + 2, &iat, 4);
    CopyMemory(c + 1, &rel, 4);
    PatchBytes((BYTE*)(ULONG_PTR)at, e, 6, c, 6);
}

static void InstallHdMovies(void)
{
    PatchCall6(0x004503B4, SMACKOPEN_IAT, SmackOpenHd);  /* call [SmackOpen] */
    PatchCall6(0x00450883, 0x0050B584u, MovieEscCheck);  /* call [GetAsyncKeyState] */
}

/* --- Mouse wheel zoom on the map -------------------------------------------
   The game has three zoom levels (Ver > Zoom) bound to Page Up ("Disminuir
   zoom") and Page Down ("Aumentar zoom"), but ignores the mouse wheel. A
   message hook on the main thread turns each wheel notch over a map window
   ("Mapa - ..." MDI child) into one of those keys. Elsewhere the wheel is
   left alone. */
static HHOOK g_wheelHook;

static BOOL IsOverMap(POINT pt)
{
    HWND w = WindowFromPoint(pt);
    char title[16];
    for (; w; w = GetParent(w)) {
        if (GetWindowThreadProcessId(w, NULL) != GetCurrentThreadId()) return FALSE;
        if (GetWindowTextA(w, title, sizeof title) >= 4 &&
            title[0] == 'M' && title[1] == 'a' && title[2] == 'p' && title[3] == 'a')
            return TRUE;
    }
    return FALSE;
}

static LRESULT CALLBACK WheelHook(int code, WPARAM wp, LPARAM lp)
{
    static int accum;
    MSG* m = (MSG*)lp;
    if (code == HC_ACTION && wp == PM_REMOVE) {
        /* Extra mouse buttons -> menu commands (IDs from the game's menus):
           middle = Info > Lista de criaturas (F5), upper side button (X2) =
           Árbol de la vida (F6), lower side button (X1) = Ver > Buscar
           (the menu says Ctrl+B; its real shortcut is Ctrl+F). Up and
           double-click messages are swallowed too. */
        UINT msg = m->message;
        if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP || msg == WM_MBUTTONDBLCLK ||
            msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP || msg == WM_XBUTTONDBLCLK) {
            HWND root = GetAncestor(m->hwnd, GA_ROOT);
            if (msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN) {
                WORD cmd = 0;
                if (msg == WM_MBUTTONDOWN) cmd = 32872;
                else if (HIWORD(m->wParam) == XBUTTON2) cmd = 32800;
                else if (HIWORD(m->wParam) == XBUTTON1) cmd = 32785;
                if (cmd && root && IsWindowEnabled(root))
                    PostMessageA(root, WM_COMMAND, cmd, 0);
            }
            m->message = WM_NULL;
        }
    }
    if (code == HC_ACTION && wp == PM_REMOVE && m->message == WM_MOUSEWHEEL) {
        POINT pt;
        pt.x = (short)LOWORD(m->lParam);
        pt.y = (short)HIWORD(m->lParam);
        if (IsOverMap(pt)) {
            HWND target = GetFocus();
            accum += (short)HIWORD(m->wParam);
            if (!target) target = m->hwnd;
            while (accum >= WHEEL_DELTA || accum <= -WHEEL_DELTA) {
                /* wheel forward: zoom in (Page Down); backward: zoom out (Page Up) */
                WPARAM vk = accum > 0 ? VK_NEXT : VK_PRIOR;
                UINT sc = MapVirtualKeyA((UINT)vk, 0);
                PostMessageA(target, WM_KEYDOWN, vk, 1 | (sc << 16) | 0x01000000);
                PostMessageA(target, WM_KEYUP, vk, 1 | (sc << 16) | 0xC1000000);
                accum += accum > 0 ? -WHEEL_DELTA : WHEEL_DELTA;
            }
            m->message = WM_NULL;
        }
    }
    return CallNextHookEx(g_wheelHook, code, wp, lp);
}

static void InstallWheelZoom(void)
{
    g_wheelHook = SetWindowsHookExA(WH_GETMESSAGE, WheelHook, NULL, GetCurrentThreadId());
}

/* --- Optional diagnostic log ----------------------------------------------
   Only active when "evo-trace.on" exists in the game folder. Logs file and
   registry opens, resource lookups and crashes to "evo-trace.log", without a
   debugger (a debugger changes the timing and hides some failures). */
typedef HANDLE (WINAPI *CreateFileAFn)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *CreateFileWFn)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef LONG (WINAPI *RegOpenKeyExAFn)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
typedef LONG (WINAPI *RegQueryValueExAFn)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
static CreateFileAFn g_realCreateFileA;
static CreateFileWFn g_realCreateFileW;
static RegOpenKeyExAFn g_realRegOpenKeyExA;
static RegQueryValueExAFn g_realRegQueryValueExA;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logcs;
static LONG g_resLogged;

static void LogLine(const char* fmt, ...)
{
    char buf[1100];
    int n;
    DWORD w;
    va_list ap;
    if (g_log == INVALID_HANDLE_VALUE) return;
    n = wsprintfA(buf, "%8lu t%04lx ", GetTickCount(), GetCurrentThreadId());
    va_start(ap, fmt);
    n += wvsprintfA(buf + n, fmt, ap);
    va_end(ap);
    buf[n++] = '\r'; buf[n++] = '\n';
    EnterCriticalSection(&g_logcs);
    WriteFile(g_log, buf, n, &w, NULL);
    LeaveCriticalSection(&g_logcs);
}

static HANDLE WINAPI HookCreateFileA(LPCSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t)
{
    HANDLE h = g_realCreateFileA(n, a, s, sa, c, f, t);
    LogLine("CreateFileA %s -> %s", n ? n : "(null)", h == INVALID_HANDLE_VALUE ? "FAIL" : "ok");
    return h;
}

static HANDLE WINAPI HookCreateFileW(LPCWSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t)
{
    HANDLE h = g_realCreateFileW(n, a, s, sa, c, f, t);
    LogLine("CreateFileW %ls -> %s", n ? n : L"(null)", h == INVALID_HANDLE_VALUE ? "FAIL" : "ok");
    return h;
}

static LONG WINAPI HookRegOpenKeyExA(HKEY k, LPCSTR sub, DWORD o, REGSAM sam, PHKEY out)
{
    LONG r = g_realRegOpenKeyExA(k, sub, o, sam, out);
    LogLine("RegOpenKeyExA %08lx %s -> %ld", (DWORD)(ULONG_PTR)k, sub ? sub : "(null)", r);
    return r;
}

static LONG WINAPI HookRegQueryValueExA(HKEY k, LPCSTR v, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD cb)
{
    LONG r = g_realRegQueryValueExA(k, v, res, type, data, cb);
    if (r == 0 && data && type && *type == REG_SZ)
        LogLine("RegQueryValueExA %s -> 0 '%s'", v ? v : "(null)", (const char*)data);
    else
        LogLine("RegQueryValueExA %s -> %ld", v ? v : "(null)", r);
    return r;
}

static void __stdcall LogResource(DWORD arg)
{
    const char* s = NULL;
    if (InterlockedIncrement(&g_resLogged) > 3000) return;
    if (!IsBadReadPtr((void*)(ULONG_PTR)arg, 4)) {
        const char* p = (const char*)(ULONG_PTR)arg;
        if (p[0] >= 0x20 && p[0] < 0x7F) s = p;
        else if (!IsBadReadPtr(*(void**)p, 4)) s = *(const char**)p;  /* CString */
    }
    LogLine("resource %s", s && !IsBadStringPtrA(s, 200) ? s : "?");
}

/* Replaces "mov eax, fs:[0]" (6 bytes) at the start of the resource lookup. */
static __declspec(naked) void ResourceEntryStub(void)
{
    __asm {
        pushad
        push dword ptr [esp + 40]
        call LogResource
        popad
        mov eax, dword ptr fs:[0]
        ret
    }
}

static LONG WINAPI LogCrash(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == 0xC0000374u || code == 0xC000041Du) {
        DWORD* sp = (DWORD*)(ULONG_PTR)ep->ContextRecord->Esp;
        char stack[400];
        int i, n = 0;
        stack[0] = 0;
        for (i = 0; i < 400 && n < 360; i++) {
            if (IsBadReadPtr(sp + i, 4)) break;
            if (sp[i] >= 0x401000u && sp[i] < 0x4F0000u)
                n += wsprintfA(stack + n, " %08lx", sp[i]);
        }
        LogLine("EXCEPTION %08lx at %08lx eax=%08lx ecx=%08lx esi=%08lx stack:%s", code,
                (DWORD)ep->ContextRecord->Eip, ep->ContextRecord->Eax, ep->ContextRecord->Ecx,
                ep->ContextRecord->Esi, stack);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void HookIat(BYTE* base, const char* dllPrefix, const char* fn, void* hook, void** real)
{
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR* d;
    if (!dir.VirtualAddress) return;
    for (d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; d++) {
        const char* dll = (const char*)(base + d->Name);
        IMAGE_THUNK_DATA* names;
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        if (CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, dll, lstrlenA(dllPrefix),
                           dllPrefix, lstrlenA(dllPrefix)) != CSTR_EQUAL)
            continue;
        if (!d->OriginalFirstThunk) continue;
        names = (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            IMAGE_IMPORT_BY_NAME* ibn;
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (lstrcmpA((const char*)ibn->Name, fn) != 0) continue;
            if (!*real) *real = (void*)(ULONG_PTR)iat->u1.Function;
            if (VirtualProtect(&iat->u1.Function, 4, PAGE_READWRITE, &old)) {
                iat->u1.Function = (DWORD)(ULONG_PTR)hook;
                VirtualProtect(&iat->u1.Function, 4, old, &old);
            }
        }
    }
}

/* Hang watchdog: when the game window stops answering, log where every
   thread is (module+offset of EIP and game return addresses on the stack). */
static void DescribeAddress(DWORD a, char* out)
{
    HMODULE m = NULL;
    char name[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(ULONG_PTR)a, &m) && GetModuleFileNameA(m, name, MAX_PATH)) {
        char* b = name + lstrlenA(name);
        while (b > name && b[-1] != '\\') b--;
        wsprintfA(out, "%s+%lx", b, a - (DWORD)(ULONG_PTR)m);
    } else {
        wsprintfA(out, "%08lx", a);
    }
}

static void DumpThreads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) do {
        HANDLE th;
        CONTEXT ctx;
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) {
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(th, &ctx)) {
                char eip[MAX_PATH + 16], stack[900];
                DWORD* sp = (DWORD*)(ULONG_PTR)ctx.Esp;
                int i, n = 0;
                stack[0] = 0;
                for (i = 0; i < 2048 && n < 820; i++) {
                    if (IsBadReadPtr(sp + i, 4)) break;
                    if (sp[i] >= 0x401000u && sp[i] < 0x4F0000u) {
                        const BYTE* r = (const BYTE*)(ULONG_PTR)sp[i];
                        n += wsprintfA(stack + n, " %08lx", sp[i]);
                        /* return address of EnterCriticalSection: call [50ACD4] or call reg */
                        if (n < 780 && !IsBadReadPtr(sp + i + 1, 4) &&
                            ((r[-6] == 0xFF && r[-5] == 0x15 && *(const DWORD*)(r - 4) == 0x0050ACD4u) ||
                             (r[-2] == 0xFF && r[-1] >= 0xD0 && r[-1] <= 0xD7))) {
                            RTL_CRITICAL_SECTION* c = (RTL_CRITICAL_SECTION*)(ULONG_PTR)sp[i + 1];
                            if (!IsBadReadPtr(c, sizeof *c) && c->OwningThread)
                                n += wsprintfA(stack + n, " [cs %08lx owner %04lx rec %ld]", (DWORD)(ULONG_PTR)c,
                                               (DWORD)(ULONG_PTR)c->OwningThread, c->RecursionCount);
                        }
                    }
                }
                DescribeAddress(ctx.Eip, eip);
                LogLine("  thread %04lx eip=%s stack:%s", te.th32ThreadID, eip, stack);
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
}

static BOOL CALLBACK FindGameWindow(HWND w, LPARAM out)
{
    DWORD pid;
    char cls[16];
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(w) && GetClassNameA(w, cls, sizeof cls) &&
        cls[0] == 'A' && cls[1] == 'f' && cls[2] == 'x') {
        *(HWND*)out = w;
        return FALSE;
    }
    return TRUE;
}

/* Movies block the window for a few seconds, which is normal; only a hang
   lasting 8 s is logged, once more at 30 s, and again for every new hang. */
static DWORD WINAPI HangWatchdog(LPVOID unused)
{
    DWORD hungSince = 0;
    int dumped = 0;
    (void)unused;
    for (;;) {
        HWND w = NULL;
        DWORD_PTR res;
        Sleep(1000);
        EnumWindows(FindGameWindow, (LPARAM)&w);
        if (!w) continue;
        if (SendMessageTimeoutA(w, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, &res)) {
            if (hungSince && dumped) LogLine("hang over after %lu ms", GetTickCount() - hungSince);
            hungSince = 0;
            dumped = 0;
            continue;
        }
        if (!hungSince) hungSince = GetTickCount();
        if ((dumped == 0 && GetTickCount() - hungSince > 8000) ||
            (dumped == 1 && GetTickCount() - hungSince > 30000)) {
            LogLine("HANG for %lu ms, threads:", GetTickCount() - hungSince);
            DumpThreads();
            dumped++;
        }
    }
}

static void InstallTrace(void)
{
    static const char* mods[] = { NULL, "MSVCRT.DLL", "MSVCIRT.DLL", "MFC42.DLL", "SMACKW32.DLL", "EVOLUTION.DLL" };
    char path[MAX_PATH + 32];
    unsigned i;
    if (!g_gameDir[0]) return;
    lstrcpyA(path, g_gameDir);
    lstrcatA(path, "evo-trace.on");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;
    InitializeCriticalSection(&g_logcs);
    lstrcpyA(path, g_gameDir);
    lstrcatA(path, "evo-trace.log");
    g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    LogLine("trace start, cmdline %s", GetCommandLineA());
    for (i = 0; i < sizeof mods / sizeof mods[0]; i++) {
        BYTE* b = (BYTE*)GetModuleHandleA(mods[i]);
        if (!b) continue;
        HookIat(b, "KERNEL32", "CreateFileA", (void*)HookCreateFileA, (void**)&g_realCreateFileA);
        HookIat(b, "KERNEL32", "CreateFileW", (void*)HookCreateFileW, (void**)&g_realCreateFileW);
        HookIat(b, "ADVAPI32", "RegOpenKeyExA", (void*)HookRegOpenKeyExA, (void**)&g_realRegOpenKeyExA);
        HookIat(b, "ADVAPI32", "RegQueryValueExA", (void*)HookRegQueryValueExA, (void**)&g_realRegQueryValueExA);
    }
    if (!g_realCreateFileA) g_realCreateFileA = (CreateFileAFn)GetProcAddress(GetModuleHandleA("kernel32"), "CreateFileA");
    if (!g_realCreateFileW) g_realCreateFileW = (CreateFileWFn)GetProcAddress(GetModuleHandleA("kernel32"), "CreateFileW");
    {
        static const BYTE e[] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 };
        BYTE c[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
        DWORD rel = (DWORD)(ULONG_PTR)ResourceEntryStub - (0x00453F00u + 5u);
        CopyMemory(c + 1, &rel, 4);
        PatchBytes((BYTE*)0x00453F00, e, sizeof e, c, sizeof c);
    }
    AddVectoredExceptionHandler(1, LogCrash);
    CloseHandle(CreateThread(NULL, 0, HangWatchdog, NULL, 0, NULL));
    LogLine("trace hooks installed");
}

/* --- Self-configuring install ----------------------------------------------
   The patched EXE reads its install settings from
   HKCU\Software\Discovery Multimedia\Evolution (the original installer wrote
   them to HKLM). They are created here, from inside the game process, so the
   folder works wherever it is copied: EXE_DIR always follows the current
   folder, the other two values are only written when missing. */
static void SetValueIfMissing(HKEY k, const char* name, const char* value, BOOL overwrite)
{
    char cur[MAX_PATH];
    DWORD cb = sizeof cur, type = 0;
    if (!overwrite && RegQueryValueExA(k, name, NULL, &type, (BYTE*)cur, &cb) == ERROR_SUCCESS)
        return;
    if (overwrite && RegQueryValueExA(k, name, NULL, &type, (BYTE*)cur, &cb) == ERROR_SUCCESS &&
        type == REG_SZ && lstrcmpiA(cur, value) == 0)
        return;
    RegSetValueExA(k, name, 0, REG_SZ, (const BYTE*)value, lstrlenA(value) + 1);
}

static void EnsureInstallSettings(void)
{
    char dir[MAX_PATH];
    int n;
    HKEY k;
    if (!g_gameDir[0]) return;
    lstrcpyA(dir, g_gameDir);
    n = lstrlenA(dir);
    if (n > 3 && dir[n - 1] == '\\') dir[n - 1] = 0;  /* EXE_DIR has no trailing backslash */
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Discovery Multimedia\\Evolution", 0, NULL, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    SetValueIfMissing(k, "EXE_DIR", dir, TRUE);
    SetValueIfMissing(k, "INSTALL_TYPE", "ALLCOLOR", FALSE);
    SetValueIfMissing(k, "CDROM_SPEED", "30", FALSE);
    RegCloseKey(k);
}

/* --- Deadlock fix ------------------------------------------------------------
   The resource loader thread (0x4552A0) holds the loader queue lock
   (CRITICAL_SECTION at 0x505E54) while it loads, and loading takes an object
   lock (0x46B170). Another thread takes that object lock first (0x46B7xx) and
   then the queue lock (0x455B20): the classic two-lock deadlock, seen after
   disasters. EnterCriticalSection calls from the game go through this wrapper.
   When a thread that holds the queue lock cannot get another lock for 100 ms,
   it gives the queue lock up until it has the other one. */
#define ENTERCS_IAT 0x0050ACD4u
#define QUEUE_CS ((LPCRITICAL_SECTION)0x00505E54u)
typedef void (WINAPI* EnterCsFn)(LPCRITICAL_SECTION);
static EnterCsFn g_realEnterCs;
static LONG g_backoffs;

static BOOL TryFor(LPCRITICAL_SECTION cs, DWORD ms)
{
    DWORD t0 = GetTickCount();
    for (;;) {
        if (TryEnterCriticalSection(cs)) return TRUE;
        if (GetTickCount() - t0 >= ms) return FALSE;
        Sleep(1);
    }
}

static void WINAPI EnterCsSafe(LPCRITICAL_SECTION cs)
{
    LPCRITICAL_SECTION q = QUEUE_CS;
    if (cs == q || (DWORD)(ULONG_PTR)q->OwningThread != GetCurrentThreadId() || q->RecursionCount != 1) {
        g_realEnterCs(cs);
        return;
    }
    if (TryFor(cs, 100)) return;
    /* Probable deadlock: let the queue lock go, take cs, then the queue lock again. */
    if (InterlockedIncrement(&g_backoffs) <= 20)
        LogLine("deadlock avoided: released queue lock to get cs %08lx (owner %04lx)",
                (DWORD)(ULONG_PTR)cs, (DWORD)(ULONG_PTR)cs->OwningThread);
    LeaveCriticalSection(q);
    for (;;) {
        g_realEnterCs(cs);
        if (TryFor(q, 100)) return;
        LeaveCriticalSection(cs);
        Sleep(2);
    }
}

static void InstallDeadlockFix(void)
{
    EnterCsFn* slot = (EnterCsFn*)(ULONG_PTR)ENTERCS_IAT;
    EnterCsFn real = (EnterCsFn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "EnterCriticalSection");
    DWORD old;
    if (IsBadReadPtr(slot, 4) || !real) return;
    /* the slot may hold kernel32's export or its forwarder target */
    g_realEnterCs = *slot;
    if (!g_realEnterCs) return;
    if (VirtualProtect(slot, 4, PAGE_READWRITE, &old)) {
        *slot = EnterCsSafe;
        VirtualProtect(slot, 4, old, &old);
    }
}

/* --- Help ---------------------------------------------------------------------
   The help is a WinHelp .HLP file, which Windows 11 cannot open. If the game
   folder has AYUDA\index.html (made from EVOLUTION.HLP by
   herramientas\ayuda_a_html.py), every WinHelp request opens that page in
   the default browser instead. */
typedef BOOL (WINAPI* WinHelpAFn)(HWND, LPCSTR, UINT, ULONG_PTR);
typedef HINSTANCE (WINAPI* ShellExecuteAFn)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
static WinHelpAFn g_realWinHelpA;

static BOOL WINAPI HookWinHelpA(HWND wnd, LPCSTR file, UINT cmd, ULONG_PTR data)
{
    char page[MAX_PATH + 32];
    HMODULE shell;
    ShellExecuteAFn open;
    if (cmd == HELP_QUIT) return TRUE;
    wsprintfA(page, "%sAYUDA\\index.html", g_gameDir);
    if (!g_gameDir[0] || GetFileAttributesA(page) == INVALID_FILE_ATTRIBUTES)
        return g_realWinHelpA ? g_realWinHelpA(wnd, file, cmd, data) : FALSE;
    shell = LoadLibraryA("shell32.dll");
    open = shell ? (ShellExecuteAFn)GetProcAddress(shell, "ShellExecuteA") : NULL;
    if (open) open(wnd, "open", page, NULL, NULL, SW_SHOWNORMAL);
    return TRUE;
}

static void InstallHelpFix(void)
{
    BYTE* exe = (BYTE*)GetModuleHandleA(NULL);
    BYTE* mfc = (BYTE*)GetModuleHandleA("MFC42.DLL");
    HookIat(exe, "USER32", "WinHelpA", (void*)HookWinHelpA, (void**)&g_realWinHelpA);
    if (mfc) HookIat(mfc, "USER32", "WinHelpA", (void*)HookWinHelpA, (void**)&g_realWinHelpA);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h; (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        g_heap = HeapCreate(0, 0, 0);
        InitializeCriticalSection(&g_cs);
        InitializeCriticalSection(&g_qcs);
        if (!g_heap) return FALSE;
        InstallMovieFixes();
        InstallNoCdFixes();      /* also sets g_gameDir */
        InstallHdMovies();
        InstallWheelZoom();
        InstallHelpFix();
        EnsureInstallSettings();
        InstallDeadlockFix();
        InstallTrace();
    }
    return TRUE;
}

/* The game has buffer overruns, frees some blocks twice and writes to blocks
   after freeing them. SmartHeap tolerated that; the Windows heap does not
   (crashes while loading music and after disasters). Every block gets a
   header with a live/dead mark and zeroed slack at its end; frees of blocks
   without a live mark are ignored, and real frees are delayed through a
   quarantine so late writes land in memory nobody else uses yet. */
#define HDR 16u
#define SLACK 64u
#define MARK_LIVE 0x45564C31u  /* "1LVE" */
#define MARK_DEAD 0x45564430u
#define QUARANTINE 8192u
static void* g_quarantine[QUARANTINE];
static unsigned g_qnext;

static DWORD* HeaderOf(void* p)
{
    return (DWORD*)((BYTE*)p - HDR);
}

static BOOL IsLive(void* p)
{
    DWORD* h = HeaderOf(p);
    return !IsBadReadPtr(h, HDR) && h[0] == MARK_LIVE;
}

void* __stdcall MemAllocPtr(void* pool, DWORD size, unsigned flags)
{
    DWORD* h;
    (void)pool; (void)flags;
    h = (DWORD*)HeapAlloc(g_heap, HEAP_ZERO_MEMORY, size + HDR + SLACK);
    if (!h) return NULL;
    h[0] = MARK_LIVE;
    h[1] = size;
    return (BYTE*)h + HDR;
}

int __stdcall MemFreePtr(void* p)
{
    DWORD* h;
    void* old;
    if (!p) return 1;
    if (!IsLive(p)) {                  /* double free or not ours: ignore */
        static LONG ignored;
        if (InterlockedIncrement(&ignored) <= 50)
            LogLine("ignored free %08lx header %08lx", (DWORD)(ULONG_PTR)p,
                    IsBadReadPtr(HeaderOf(p), 4) ? 0 : HeaderOf(p)[0]);
        return 0;
    }
    h = HeaderOf(p);
    h[0] = MARK_DEAD;
    EnterCriticalSection(&g_qcs);
    old = g_quarantine[g_qnext];
    g_quarantine[g_qnext] = h;
    g_qnext = (g_qnext + 1) % QUARANTINE;
    LeaveCriticalSection(&g_qcs);
    if (old) HeapFree(g_heap, 0, old);
    return 1;
}

void* __stdcall MemReAllocPtr(void* p, DWORD size, unsigned flags)
{
    DWORD* h;
    if (!p) return MemAllocPtr(0, size, flags);
    if (!IsLive(p)) return MemAllocPtr(0, size, flags);
    /* Flag 8 is used by the game's _expand wrapper: the block must not move. */
    h = (DWORD*)HeapReAlloc(g_heap,
                            HEAP_ZERO_MEMORY | ((flags & 8u) ? HEAP_REALLOC_IN_PLACE_ONLY : 0),
                            HeaderOf(p), size + HDR + SLACK);
    if (!h) return NULL;
    h[1] = size;
    return (BYTE*)h + HDR;
}

DWORD __stdcall MemSizePtr(void* p)
{
    if (!p || !IsLive(p)) return (DWORD)-1;
    return HeaderOf(p)[1];
}

int __stdcall MemCheckPtr(void* pool, void* p)
{
    (void)pool;
    return p && IsLive(p) ? 1 : 0;
}

void* __stdcall MemPoolInit(unsigned flags)
{
    (void)flags;
    return (void*)g_heap;
}

DWORD __stdcall MemPoolSetPageSize(void* pool, DWORD size)
{
    (void)pool;
    return size;
}

DWORD __stdcall MemPoolSetBlockSizeFS(void* pool, DWORD size)
{
    (void)pool;
    return size;
}

DWORD __stdcall MemPoolShrink(void* pool)
{
    (void)pool;
    return 0;
}

DWORD __stdcall MemPoolSize(void* pool)
{
    (void)pool;
    return 0;
}

int __stdcall MemPoolCheck(void* pool)
{
    (void)pool;
    return 1;
}

int __stdcall MemPoolWalk(void* pool, void* entry)
{
    (void)pool; (void)entry;
    return 0;
}

/* The game's pool initializer treats a zero return as failure. */
int __cdecl _shi_enterCriticalSection(void) { EnterCriticalSection(&g_cs); return 1; }
int __cdecl _shi_leaveCriticalSection(void) { LeaveCriticalSection(&g_cs); return 1; }
