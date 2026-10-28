// FH2NoPause: keeps Forza Horizon 2 running when its window loses focus.
//
// Two layers, because it isn't known which one the game checks:
//   - the game window's WndProc swallows "you lost focus" messages
//     (WM_ACTIVATEAPP false, WM_ACTIVATE inactive, WM_KILLFOCUS), and
//   - GetForegroundWindow / GetActiveWindow / GetFocus answer with the game
//     window while the setting is on.
// Azimuth reads the pad through Windows.Gaming.Input, which stops delivering readings
// to a window that isn't in the foreground. So IGamepad::GetCurrentReading is hooked, and
// while the game is unfocused its reading is filled from XInput (which isn't gated).
// Turn it all off in Pause > Settings > MODS, or with KeepRunning=0 in FH2NoPause.ini.
#include "fh2mod.h"

#include <windows.h>
#include <roapi.h>
#include <winstring.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

const FH2ModApi* g_api = nullptr;
FH2Mod* g_mod          = nullptr;
std::atomic<bool> g_keep{true};
std::atomic<HWND> g_hwnd{nullptr};   // the game window we subclassed
wchar_t g_ini[MAX_PATH]              = L"";
WNDPROC g_orig_proc                  = nullptr;

HWND(WINAPI* g_orig_foreground)() = nullptr;
HWND(WINAPI* g_orig_active)()     = nullptr;
HWND(WINAPI* g_orig_focus)()      = nullptr;

void log(const char* fmt, ...);

// ---- controller -----------------------------------------------------------------------

// Windows.Gaming.Input.GamepadReading
struct GamepadReading {
    UINT64 timestamp;
    UINT32 buttons;
    double left_trigger, right_trigger;
    double lx, ly, rx, ry;
};

struct XState {
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};

DWORD(WINAPI* g_xinput_get)(DWORD, XState*)                    = nullptr;
HRESULT(STDMETHODCALLTYPE* g_orig_reading)(void*, GamepadReading*) = nullptr;
std::atomic<long> g_reads_total{0}, g_reads_unfocused{0}, g_reads_xinput{0};

UINT32 map_buttons(WORD b) {
    static const struct { WORD x; UINT32 g; } m[] = {
        {0x0010, 0x1},   {0x0020, 0x2},   {0x1000, 0x4},   {0x2000, 0x8},   {0x4000, 0x10},  {0x8000, 0x20},
        {0x0001, 0x40},  {0x0002, 0x80},  {0x0004, 0x100}, {0x0008, 0x200}, {0x0100, 0x400}, {0x0200, 0x800},
        {0x0040, 0x1000}, {0x0080, 0x2000},
    };
    UINT32 r = 0;
    for (auto& e : m)
        if (b & e.x) r |= e.g;
    return r;
}

double axis(SHORT v) { return v < 0 ? v / 32768.0 : v / 32767.0; }

bool game_unfocused() {
    HWND h = g_hwnd.load();
    return h && g_orig_foreground && g_orig_foreground() != h;
}

HRESULT STDMETHODCALLTYPE detour_reading(void* self, GamepadReading* out) {
    HRESULT hr = g_orig_reading(self, out);
    long n     = ++g_reads_total;
    if (!g_keep.load() || !g_xinput_get || !out || !game_unfocused()) return hr;
    long u = ++g_reads_unfocused;
    if (u == 1 || u % 2000 == 0)
        log("unfocused pad read #%ld (hr=%08lX, buttons=%X, lx=%.2f)", u, static_cast<unsigned long>(hr), out->buttons, out->lx);
    for (DWORD i = 0; i < 4; ++i) {
        XState st{};
        if (g_xinput_get(i, &st) != ERROR_SUCCESS) continue;
        out->buttons       = map_buttons(st.buttons);
        out->left_trigger  = st.lt / 255.0;
        out->right_trigger = st.rt / 255.0;
        out->lx            = axis(st.lx);
        out->ly            = axis(st.ly);
        out->rx            = axis(st.rx);
        out->ry            = axis(st.ry);
        if (++g_reads_xinput == 1) log("feeding XInput pad %lu into unfocused readings (of %ld reads)", i, n);
        return S_OK;
    }
    return hr;
}

// Find IGamepad::GetCurrentReading (vtable slot 8) via the Gamepad statics and hook it.
// Needs one connected pad to get an instance; the code is shared by all pads.
DWORD WINAPI pad_hook_thread(LPVOID) {
    HMODULE xi = LoadLibraryW(L"xinput1_4.dll");
    if (!xi) xi = LoadLibraryW(L"xinput9_1_0.dll");
    if (xi) g_xinput_get = reinterpret_cast<decltype(g_xinput_get)>(GetProcAddress(xi, "XInputGetState"));
    if (!g_xinput_get) {
        log("no XInput available, pad stays foreground-only");
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    static const GUID iid_statics = {0x8BBCE529, 0xD49C, 0x39E9, {0x95, 0x60, 0xE4, 0x7D, 0xDE, 0x96, 0xB7, 0xC8}};
    HSTRING cls;
    HSTRING_HEADER hdr;
    const wchar_t name[] = L"Windows.Gaming.Input.Gamepad";
    if (FAILED(WindowsCreateStringReference(name, static_cast<UINT32>(wcslen(name)), &hdr, &cls))) return 0;

    for (int tries = 0; tries < 600; ++tries, Sleep(1000)) {
        void** statics = nullptr;
        if (FAILED(RoGetActivationFactory(cls, iid_statics, reinterpret_cast<void**>(&statics))) || !statics) continue;
        using Fn1 = HRESULT(STDMETHODCALLTYPE*)(void*, void**);
        using Fn2 = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32*);
        using Fn3 = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32, void**);
        void** vec = nullptr;
        // IGamepadStatics: 6 add_GamepadAdded, 7 remove, 8 add_GamepadRemoved, 9 remove, 10 get_Gamepads
        HRESULT hr = reinterpret_cast<Fn1>((*reinterpret_cast<void***>(statics))[10])(statics, reinterpret_cast<void**>(&vec));
        void** pad = nullptr;
        UINT32 size = 0;
        if (SUCCEEDED(hr) && vec) {
            // IVectorView: 6 GetAt, 7 get_Size
            reinterpret_cast<Fn2>((*reinterpret_cast<void***>(vec))[7])(vec, &size);
            if (size) reinterpret_cast<Fn3>((*reinterpret_cast<void***>(vec))[6])(vec, 0, reinterpret_cast<void**>(&pad));
        }
        if (pad) {
            void* target = (*reinterpret_cast<void***>(pad))[8];   // IGamepad::GetCurrentReading
            log("Gamepad::GetCurrentReading at %p", target);
            FH2Hook* h = g_api->hook_function(g_mod, "IGamepad::GetCurrentReading", target, reinterpret_cast<void*>(&detour_reading),
                                              reinterpret_cast<void**>(&g_orig_reading));
            if (h) g_api->enable_hooks(g_mod, h);
            return 0;
        }
    }
    log("no Windows.Gaming.Input gamepad found in 10 minutes; pad hook not installed");
    return 0;
}

void log(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_api->log(g_mod, buf);
}

void init_paths() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&init_paths), &self);
    GetModuleFileNameW(self, g_ini, MAX_PATH);
    wchar_t* slash = wcsrchr(g_ini, L'\\');
    if (slash) *(slash + 1) = 0;
    wcscat_s(g_ini, L"FH2NoPause.ini");
}

void save_config() { WritePrivateProfileStringW(L"Settings", L"KeepRunning", g_keep.load() ? L"1" : L"0", g_ini); }

void on_setting_changed(void*, const char* key, int value) {
    if (strcmp(key, "KeepRunning") != 0) return;
    g_keep.store(value != 0);
    save_config();
    log(value ? "Game keeps running when unfocused" : "Game pauses when unfocused again");
}

LRESULT CALLBACK game_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_keep.load()) {
        switch (msg) {
        case WM_ACTIVATEAPP:
            if (!wp) return 0;
            break;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) return 0;
            break;
        case WM_KILLFOCUS:
            return 0;
        default:
            break;
        }
    }
    return CallWindowProcW(g_orig_proc, hwnd, msg, wp, lp);
}

HWND WINAPI detour_foreground() {
    HWND h = g_hwnd.load();
    if (g_keep.load() && h) return h;
    return g_orig_foreground();
}
HWND WINAPI detour_active() {
    HWND h = g_hwnd.load();
    if (g_keep.load() && h) return h;
    return g_orig_active();
}
HWND WINAPI detour_focus() {
    HWND h = g_hwnd.load();
    if (g_keep.load() && h) return h;
    return g_orig_focus();
}

// The game's window: the biggest visible top-level window of this process.
// Tool windows and click-through overlays (like the nitro debug overlay) don't count.
struct Find {
    HWND best = nullptr;
    long area = 0;
};
BOOL CALLBACK enum_proc(HWND hw, LPARAM lp) {
    auto* f = reinterpret_cast<Find*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hw, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hw) || GetWindow(hw, GW_OWNER)) return TRUE;
    LONG_PTR ex = GetWindowLongPtrW(hw, GWL_EXSTYLE);
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) return TRUE;
    RECT r;
    if (!GetClientRect(hw, &r)) return TRUE;
    long a = (r.right - r.left) * (r.bottom - r.top);
    if (a > f->area) {
        f->area = a;
        f->best = hw;
    }
    return TRUE;
}

void subclass(HWND hw) {
    if (hw == g_hwnd.load()) return;
    WNDPROC cur = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hw, GWLP_WNDPROC));
    if (cur == game_proc) return;
    g_orig_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hw, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(game_proc)));
    if (!g_orig_proc) {
        log("couldn't subclass the game window (error %lu)", GetLastError());
        return;
    }
    g_hwnd.store(hw);
    wchar_t title[96] = L"", cls[96] = L"";
    GetWindowTextW(hw, title, 96);
    GetClassNameW(hw, cls, 96);
    log("watching game window %p (class %ls, title \"%ls\")", static_cast<void*>(hw), cls, title);
}

DWORD WINAPI watcher(LPVOID) {
    // The window comes and goes (loading, mode switches), so keep checking.
    for (;;) {
        Find f;
        EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&f));
        if (f.best) {
            HWND cur = g_hwnd.load();
            // One window at a time (g_orig_proc is shared): move over only when ours is gone
            if (!cur || !IsWindow(cur)) subclass(f.best);
        }
        Sleep(1500);
    }
    return 0;
}

void make_hook(const char* name, const char* fn, void* detour, void** original) {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    void* target = u32 ? reinterpret_cast<void*>(GetProcAddress(u32, fn)) : nullptr;
    if (!target) {
        log("%s not found", fn);
        return;
    }
    g_api->hook_function(g_mod, name, target, detour, original);
}

} // namespace

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    if (api->api_version < 3) return 1;
    g_api = api;

    FH2ModInfo info{};
    info.size             = sizeof info;
    info.name             = "NoPause";
    info.version          = "1.1.1";
    info.github_repo      = "qiangqiang101/FH2NoPauseWhenLosingFocus";
    info.platform_version = FH2MOD_PLATFORM_VERSION;
    info.check_updates    = 1;
    info.description      = "Keeps the game running when its window loses focus (alt-tab)";
    g_mod                 = api->register_mod(&info);
    if (!g_mod) return 1;

    init_paths();
    g_keep.store(GetPrivateProfileIntW(L"Settings", L"KeepRunning", 1, g_ini) != 0);

    if (api->api_version >= 4) {
        FH2Setting s{};
        s.size        = sizeof s;
        s.key         = "KeepRunning";
        s.label       = "Keep Running When Unfocused";
        s.description = "Stops the game pausing when you alt-tab or click another window.";
        s.value       = g_keep.load() ? 1 : 0;
        s.on_change   = on_setting_changed;
        api->add_setting(g_mod, &s);
    }

    make_hook("user32!GetForegroundWindow", "GetForegroundWindow", reinterpret_cast<void*>(&detour_foreground),
              reinterpret_cast<void**>(&g_orig_foreground));
    make_hook("user32!GetActiveWindow", "GetActiveWindow", reinterpret_cast<void*>(&detour_active),
              reinterpret_cast<void**>(&g_orig_active));
    make_hook("user32!GetFocus", "GetFocus", reinterpret_cast<void*>(&detour_focus), reinterpret_cast<void**>(&g_orig_focus));
    api->enable_hooks(g_mod, nullptr);

    if (HANDLE t = CreateThread(nullptr, 0, pad_hook_thread, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, watcher, nullptr, 0, nullptr)) CloseHandle(t);
    log("NoPause ready (%s)", g_keep.load() ? "on" : "off");
    return 0;
}
