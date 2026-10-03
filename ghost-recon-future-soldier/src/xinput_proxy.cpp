// xinput1_3.dll proxy for Ghost Recon: Future Soldier.
//
// The game loads this file instead of Windows' XInput because it sits next to
// the game's .exe files. Every call is passed through to the real system
// XInput DLL, loaded only by its full System32 path. The only change made is
// flipping the stick axes chosen in xinput_proxy.ini (by default the right
// stick's up/down, so looking is inverted).
//
// Steam injects gameoverlayrenderer.dll into games (even with Steam Input and
// the overlay turned off) and patches the start of XInput functions so its own
// code reads the controller instead. To stay in the path, this DLL points the
// game's XInput imports directly at private copies of its functions, which
// nothing else knows about ([Compatibility] RouteAroundHooks=1).
//
// With LogStats=1 it also logs poll statistics and, for the first two minutes,
// where the game's XInput calls actually go.
//
// It makes no network calls, writes no registry keys and starts no processes.
// The only file it writes is xinput_proxy.log next to itself.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <xinput.h>
#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace {

HMODULE g_self = nullptr;
HMODULE g_real = nullptr;
std::once_flag g_initOnce;
std::mutex g_logMutex;
FILE* g_log = nullptr;
bool g_loggedFirstState = false;

bool g_invLX = false, g_invLY = false, g_invRX = false, g_invRY = true;
bool g_routeAround = true;

// Optional periodic summary of what the game asks for ([General] LogStats=1).
bool g_logStats = false;
struct Stats {
  DWORD windowStart = 0;
  unsigned calls = 0, ok = 0, notConnected = 0, exCalls = 0;
  SHORT rawMin = 0, rawMax = 0;
  bool any = false;
};
Stats g_stats;
std::mutex g_statsMutex;
volatile LONG g_totalCalls = 0;

using PFN_GetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using PFN_SetState = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
using PFN_GetCapabilities = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
using PFN_Enable = void(WINAPI*)(BOOL);
using PFN_GetDSoundAudioDeviceGuids = DWORD(WINAPI*)(DWORD, GUID*, GUID*);
using PFN_GetBatteryInformation = DWORD(WINAPI*)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);
using PFN_GetKeystroke = DWORD(WINAPI*)(DWORD, DWORD, PXINPUT_KEYSTROKE);
using PFN_WaitForGuideButton = DWORD(WINAPI*)(DWORD, DWORD, void*);
using PFN_CancelGuideButtonWait = DWORD(WINAPI*)(DWORD);
using PFN_PowerOffController = DWORD(WINAPI*)(DWORD);

PFN_GetState g_getState = nullptr;
PFN_GetState g_getStateEx = nullptr;  // hidden ordinal 100, which also reports the Guide button
PFN_SetState g_setState = nullptr;
PFN_GetCapabilities g_getCapabilities = nullptr;
PFN_Enable g_enable = nullptr;
PFN_GetDSoundAudioDeviceGuids g_getDSoundGuids = nullptr;
PFN_GetBatteryInformation g_getBattery = nullptr;
PFN_GetKeystroke g_getKeystroke = nullptr;
PFN_WaitForGuideButton g_waitForGuide = nullptr;
PFN_CancelGuideButtonWait g_cancelGuideWait = nullptr;
PFN_PowerOffController g_powerOff = nullptr;

// ------------------------------------------------------------------ helpers

void Log(const char* fmt, ...) {
  std::lock_guard<std::mutex> lock(g_logMutex);
  if (!g_log) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(g_log, "%02u:%02u:%02u.%03u  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
}

std::string Utf8(const wchar_t* s) {
  int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr);
  return out;
}

void Init() {
  std::call_once(g_initOnce, [] {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring dir = path;
    dir.resize(dir.find_last_of(L"\\/") + 1);
    const std::wstring ini = dir + L"xinput_proxy.ini";

    if (GetPrivateProfileIntW(L"General", L"Log", 1, ini.c_str()) != 0)
      g_log = _wfsopen((dir + L"xinput_proxy.log").c_str(), L"w", _SH_DENYWR);
    g_invLX = GetPrivateProfileIntW(L"Invert", L"LeftX", 0, ini.c_str()) != 0;
    g_invLY = GetPrivateProfileIntW(L"Invert", L"LeftY", 0, ini.c_str()) != 0;
    g_invRX = GetPrivateProfileIntW(L"Invert", L"RightX", 0, ini.c_str()) != 0;
    g_invRY = GetPrivateProfileIntW(L"Invert", L"RightY", 1, ini.c_str()) != 0;
    g_logStats = g_log && GetPrivateProfileIntW(L"General", L"LogStats", 0, ini.c_str()) != 0;
    g_routeAround = GetPrivateProfileIntW(L"Compatibility", L"RouteAroundHooks", 1, ini.c_str()) != 0;

    GetModuleFileNameW(nullptr, path, MAX_PATH);
    Log("xinput proxy loaded into %s", Utf8(path).c_str());
    Log("Inverting: LeftX=%d LeftY=%d RightX=%d RightY=%d", g_invLX, g_invLY, g_invRX, g_invRY);

    // Full path only: never search the game folder or PATH for the real DLL.
    wchar_t sys[MAX_PATH] = {};
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    for (const wchar_t* name : {L"xinput1_3.dll", L"xinput1_4.dll"}) {
      std::wstring real = std::wstring(sys, n) + L"\\" + name;
      g_real = LoadLibraryExW(real.c_str(), nullptr, 0);
      if (g_real == g_self) g_real = nullptr;
      if (g_real) break;
    }
    if (!g_real) {
      Log("FATAL: could not load the real XInput DLL from %s (error %lu)", Utf8(sys).c_str(), GetLastError());
      return;
    }
    GetModuleFileNameW(g_real, path, MAX_PATH);
    Log("Real XInput: %s", Utf8(path).c_str());

    g_getState = reinterpret_cast<PFN_GetState>(GetProcAddress(g_real, "XInputGetState"));
    g_getStateEx = reinterpret_cast<PFN_GetState>(GetProcAddress(g_real, MAKEINTRESOURCEA(100)));
    g_setState = reinterpret_cast<PFN_SetState>(GetProcAddress(g_real, "XInputSetState"));
    g_getCapabilities = reinterpret_cast<PFN_GetCapabilities>(GetProcAddress(g_real, "XInputGetCapabilities"));
    g_enable = reinterpret_cast<PFN_Enable>(GetProcAddress(g_real, "XInputEnable"));
    g_getDSoundGuids =
        reinterpret_cast<PFN_GetDSoundAudioDeviceGuids>(GetProcAddress(g_real, "XInputGetDSoundAudioDeviceGuids"));
    g_getBattery = reinterpret_cast<PFN_GetBatteryInformation>(GetProcAddress(g_real, "XInputGetBatteryInformation"));
    g_getKeystroke = reinterpret_cast<PFN_GetKeystroke>(GetProcAddress(g_real, "XInputGetKeystroke"));
    g_waitForGuide = reinterpret_cast<PFN_WaitForGuideButton>(GetProcAddress(g_real, MAKEINTRESOURCEA(101)));
    g_cancelGuideWait = reinterpret_cast<PFN_CancelGuideButtonWait>(GetProcAddress(g_real, MAKEINTRESOURCEA(102)));
    g_powerOff = reinterpret_cast<PFN_PowerOffController>(GetProcAddress(g_real, MAKEINTRESOURCEA(103)));
  });
}

SHORT Flip(SHORT v) { return v == -32768 ? 32767 : static_cast<SHORT>(-v); }

// Every 3 seconds, log how often the game polled and the right stick's raw Y range.
void RecordStats(DWORD user, DWORD result, const XINPUT_STATE* s, bool ex) {
  if (!g_logStats) return;
  InterlockedIncrement(&g_totalCalls);
  std::lock_guard<std::mutex> lock(g_statsMutex);
  Stats& st = g_stats;
  DWORD now = GetTickCount();
  if (st.windowStart == 0) st.windowStart = now;
  if (now - st.windowStart >= 3000) {
    if (st.any)
      Log("last 3s: %u GetState + %u GetStateEx calls, %u connected, %u not connected; right stick Y raw %d..%d, "
          "game got %d..%d",
          st.calls - st.exCalls, st.exCalls, st.ok, st.notConnected, st.rawMin, st.rawMax,
          g_invRY ? Flip(st.rawMax) : st.rawMin, g_invRY ? Flip(st.rawMin) : st.rawMax);
    else
      Log("last 3s: %u GetState + %u GetStateEx calls, %u connected, %u not connected", st.calls - st.exCalls,
          st.exCalls, st.ok, st.notConnected);
    st = Stats{};
    st.windowStart = now;
  }
  ++st.calls;
  if (ex) ++st.exCalls;
  if (result == ERROR_SUCCESS && s) {
    ++st.ok;
    SHORT y = s->Gamepad.sThumbRY;
    if (!st.any || y < st.rawMin) st.rawMin = y;
    if (!st.any || y > st.rawMax) st.rawMax = y;
    st.any = true;
  } else if (result == ERROR_DEVICE_NOT_CONNECTED && user == 0) {
    ++st.notConnected;
  }
}

void ApplyInvert(DWORD user, DWORD result, XINPUT_STATE* s) {
  if (result != ERROR_SUCCESS || !s) return;
  if (!g_loggedFirstState) {
    g_loggedFirstState = true;
    Log("Game is reading controller %lu", user);
  }
  XINPUT_GAMEPAD& p = s->Gamepad;
  if (g_invLX) p.sThumbLX = Flip(p.sThumbLX);
  if (g_invLY) p.sThumbLY = Flip(p.sThumbLY);
  if (g_invRX) p.sThumbRX = Flip(p.sThumbRX);
  if (g_invRY) p.sThumbRY = Flip(p.sThumbRY);
}

// Keystroke events report stick directions as virtual keys; keep them consistent.
WORD FlipStickKey(WORD vk) {
  if (g_invRY) {
    switch (vk) {
      case VK_PAD_RTHUMB_UP: return VK_PAD_RTHUMB_DOWN;
      case VK_PAD_RTHUMB_DOWN: return VK_PAD_RTHUMB_UP;
      case VK_PAD_RTHUMB_UPLEFT: return VK_PAD_RTHUMB_DOWNLEFT;
      case VK_PAD_RTHUMB_DOWNLEFT: return VK_PAD_RTHUMB_UPLEFT;
      case VK_PAD_RTHUMB_UPRIGHT: return VK_PAD_RTHUMB_DOWNRIGHT;
      case VK_PAD_RTHUMB_DOWNRIGHT: return VK_PAD_RTHUMB_UPRIGHT;
    }
  }
  if (g_invLY) {
    switch (vk) {
      case VK_PAD_LTHUMB_UP: return VK_PAD_LTHUMB_DOWN;
      case VK_PAD_LTHUMB_DOWN: return VK_PAD_LTHUMB_UP;
      case VK_PAD_LTHUMB_UPLEFT: return VK_PAD_LTHUMB_DOWNLEFT;
      case VK_PAD_LTHUMB_DOWNLEFT: return VK_PAD_LTHUMB_UPLEFT;
      case VK_PAD_LTHUMB_UPRIGHT: return VK_PAD_LTHUMB_DOWNRIGHT;
      case VK_PAD_LTHUMB_DOWNRIGHT: return VK_PAD_LTHUMB_UPRIGHT;
    }
  }
  return vk;
}

// ---------------------------------------------------------- implementations
// The real work lives in these private functions. The exported functions just
// forward here, so a patch on an export can be routed around by pointing the
// game straight at these. noinline keeps each one a separate, unpatched copy.

__declspec(noinline) DWORD WINAPI Impl_GetState(DWORD user, XINPUT_STATE* state) {
  Init();
  if (!g_getState) return ERROR_DEVICE_NOT_CONNECTED;
  DWORD r = g_getState(user, state);
  RecordStats(user, r, state, false);
  ApplyInvert(user, r, state);
  return r;
}

__declspec(noinline) DWORD WINAPI Impl_GetStateEx(DWORD user, XINPUT_STATE* state) {
  Init();
  PFN_GetState f = g_getStateEx ? g_getStateEx : g_getState;
  if (!f) return ERROR_DEVICE_NOT_CONNECTED;
  DWORD r = f(user, state);
  RecordStats(user, r, state, true);
  ApplyInvert(user, r, state);
  return r;
}

__declspec(noinline) DWORD WINAPI Impl_SetState(DWORD user, XINPUT_VIBRATION* vibration) {
  Init();
  return g_setState ? g_setState(user, vibration) : ERROR_DEVICE_NOT_CONNECTED;
}

__declspec(noinline) DWORD WINAPI Impl_GetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
  Init();
  return g_getCapabilities ? g_getCapabilities(user, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
}

__declspec(noinline) void WINAPI Impl_Enable(BOOL enable) {
  Init();
  if (g_enable) g_enable(enable);
}

__declspec(noinline) DWORD WINAPI Impl_GetDSoundAudioDeviceGuids(DWORD user, GUID* render, GUID* capture) {
  Init();
  if (g_getDSoundGuids) return g_getDSoundGuids(user, render, capture);
  if (render) *render = GUID{};
  if (capture) *capture = GUID{};
  return ERROR_SUCCESS;  // same as having no headset, which is what XInput 1.4 systems report
}

__declspec(noinline) DWORD WINAPI Impl_GetBatteryInformation(DWORD user, BYTE type, XINPUT_BATTERY_INFORMATION* info) {
  Init();
  return g_getBattery ? g_getBattery(user, type, info) : ERROR_DEVICE_NOT_CONNECTED;
}

__declspec(noinline) DWORD WINAPI Impl_GetKeystroke(DWORD user, DWORD reserved, PXINPUT_KEYSTROKE ks) {
  Init();
  if (!g_getKeystroke) return ERROR_DEVICE_NOT_CONNECTED;
  DWORD r = g_getKeystroke(user, reserved, ks);
  if (r == ERROR_SUCCESS && ks) ks->VirtualKey = FlipStickKey(ks->VirtualKey);
  return r;
}

__declspec(noinline) DWORD WINAPI Impl_WaitForGuideButton(DWORD user, DWORD flags, void* listen) {
  Init();
  return g_waitForGuide ? g_waitForGuide(user, flags, listen) : ERROR_DEVICE_NOT_CONNECTED;
}

__declspec(noinline) DWORD WINAPI Impl_CancelGuideButtonWait(DWORD user) {
  Init();
  return g_cancelGuideWait ? g_cancelGuideWait(user) : ERROR_DEVICE_NOT_CONNECTED;
}

__declspec(noinline) DWORD WINAPI Impl_PowerOffController(DWORD user) {
  Init();
  return g_powerOff ? g_powerOff(user) : ERROR_DEVICE_NOT_CONNECTED;
}

// The private function for an XInput import, by ordinal or by name.
const void* ImplFor(WORD ordinal, const char* name) {
  struct Entry {
    WORD ordinal;
    const char* name;
    const void* fn;
  };
  static const Entry kTable[] = {
      {2, "XInputGetState", reinterpret_cast<const void*>(&Impl_GetState)},
      {3, "XInputSetState", reinterpret_cast<const void*>(&Impl_SetState)},
      {4, "XInputGetCapabilities", reinterpret_cast<const void*>(&Impl_GetCapabilities)},
      {5, "XInputEnable", reinterpret_cast<const void*>(&Impl_Enable)},
      {6, "XInputGetDSoundAudioDeviceGuids", reinterpret_cast<const void*>(&Impl_GetDSoundAudioDeviceGuids)},
      {7, "XInputGetBatteryInformation", reinterpret_cast<const void*>(&Impl_GetBatteryInformation)},
      {8, "XInputGetKeystroke", reinterpret_cast<const void*>(&Impl_GetKeystroke)},
      {100, nullptr, reinterpret_cast<const void*>(&Impl_GetStateEx)},
      {101, nullptr, reinterpret_cast<const void*>(&Impl_WaitForGuideButton)},
      {102, nullptr, reinterpret_cast<const void*>(&Impl_CancelGuideButtonWait)},
      {103, nullptr, reinterpret_cast<const void*>(&Impl_PowerOffController)},
  };
  for (const Entry& e : kTable)
    if (name ? (e.name && strcmp(e.name, name) == 0) : e.ordinal == ordinal) return e.fn;
  return nullptr;
}

// ------------------------------------------------------ import table access

// Calls visit(label, iat slot, ordinal, name) for each XInput import of the game's .exe.
template <class Visit>
void ForEachXInputImport(Visit visit) {
  BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return;
  for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
    const char* dll = reinterpret_cast<const char*>(base + d->Name);
    if (_strnicmp(dll, "xinput", 6) != 0) continue;
    auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
    auto* names = d->OriginalFirstThunk ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk) : nullptr;
    for (int i = 0; iat[i].u1.Function; ++i) {
      WORD ordinal = 0;
      const char* name = nullptr;
      char label[96];
      if (names && IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) {
        ordinal = static_cast<WORD>(IMAGE_ORDINAL(names[i].u1.Ordinal));
        snprintf(label, sizeof label, "%s ordinal %u", dll, ordinal);
      } else if (names) {
        name = reinterpret_cast<const char*>(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData)->Name);
        snprintf(label, sizeof label, "%s %s", dll, name);
      } else {
        snprintf(label, sizeof label, "%s entry %d", dll, i);
      }
      visit(label, &iat[i].u1.Function, ordinal, name, names != nullptr);
    }
  }
}

// Point the game's XInput imports at the private functions. Returns what changed.
std::string RouteGameImports() {
  std::string changed;
  ForEachXInputImport([&](const char* label, ULONG_PTR* slot, WORD ordinal, const char* name, bool known) {
    if (!known) return;
    const void* fn = ImplFor(ordinal, name);
    if (!fn || *slot == reinterpret_cast<ULONG_PTR>(fn)) return;
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return;
    InterlockedExchangePointer(reinterpret_cast<PVOID*>(slot), const_cast<void*>(fn));
    VirtualProtect(slot, sizeof *slot, old, &old);
    changed += std::string("\n    ") + label;
  });
  return changed;
}

// ------------------------------------------------------------ diagnostics

std::string ModuleOf(const void* p) {
  HMODULE m = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(p), &m))
    return "unowned memory";
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(m, path, MAX_PATH);
  const wchar_t* slash = wcsrchr(path, L'\\');
  return Utf8(slash ? slash + 1 : path);
}

bool Readable(const void* p, size_t n) {
  MEMORY_BASIC_INFORMATION mbi = {};
  if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
    return false;
  return static_cast<const BYTE*>(p) + n <= static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
}

// If code starts with a jump, return where it goes (common hook-stub shapes only).
const void* JumpTarget(const BYTE* code) {
  if (!Readable(code, 8)) return nullptr;
  if (code[0] == 0xE9) {  // jmp rel32
    LONG rel;
    memcpy(&rel, code + 1, sizeof rel);
    return code + 5 + rel;
  }
  if (code[0] == 0xFF && code[1] == 0x25) {  // jmp [abs32]
    void** slot;
    memcpy(&slot, code + 2, sizeof slot);
    return Readable(slot, sizeof(void*)) ? *slot : nullptr;
  }
  if (code[0] == 0x68 && code[5] == 0xC3) {  // push imm32; ret
    void* t;
    memcpy(&t, code + 1, sizeof t);
    return t;
  }
  if (code[0] == 0xB8 && code[5] == 0xFF && code[6] == 0xE0) {  // mov eax, imm32; jmp eax
    void* t;
    memcpy(&t, code + 1, sizeof t);
    return t;
  }
  return nullptr;
}

// Describe where a function pointer really leads, following up to 4 patched jumps.
std::string Describe(const void* fn) {
  std::string out = ModuleOf(fn);
  const BYTE* code = static_cast<const BYTE*>(fn);
  for (int hop = 0; hop < 4; ++hop) {
    const void* next = JumpTarget(code);
    if (!next) break;
    out += " -> jumps to " + ModuleOf(next);
    code = static_cast<const BYTE*>(next);
  }
  if (g_self && fn) {
    for (int ord : {2, 3, 4, 100})
      if (fn == ImplFor(static_cast<WORD>(ord), nullptr)) out += " (this DLL's private copy)";
  }
  return out;
}

std::string ImportReport() {
  std::string report;
  ForEachXInputImport([&](const char* label, ULONG_PTR* slot, WORD, const char*, bool) {
    report += std::string("\n    game's ") + label + " -> " + Describe(reinterpret_cast<const void*>(*slot));
  });
  return report;
}

std::string ModuleReport() {
  HMODULE mods[1024];
  DWORD needed = 0;
  std::string report;
  if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) return report;
  for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; ++i) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(mods[i], path, MAX_PATH);
    std::wstring lower = path;
    CharLowerBuffW(&lower[0], static_cast<DWORD>(lower.size()));
    for (const wchar_t* key : {L"xinput", L"overlay", L"steam", L"dinput", L"hid.dll", L"gameinput", L"uplay", L"ubisoft"})
      if (lower.find(key) != std::wstring::npos) {
        report += "\n    " + Utf8(path);
        break;
      }
  }
  return report;
}

// Started from DllMain, so the imports are rerouted even if the game's first
// XInput call never reaches this DLL. Rechecks every 2 seconds for 2 minutes,
// then every 30 seconds, in case something patches the import table later.
DWORD WINAPI WatchThread(LPVOID) {
  Init();
  if (!g_routeAround && !g_logStats) return 0;
  std::string lastImports, lastModules;
  LONG lastCalls = -1;
  bool first = true;
  for (int tick = 0;; ++tick) {
    if (g_routeAround && g_real) {
      std::string changed = RouteGameImports();
      if (!changed.empty())
        Log(first ? "Pointed the game's XInput imports at this DLL's private functions:%s"
                  : "Something changed the game's XInput imports again; pointed back:%s",
            changed.c_str());
    }
    first = false;
    if (g_logStats && tick < 60) {
      std::string imports = ImportReport();
      if (imports != lastImports) {
        Log("Where the game's XInput calls go now:%s", imports.c_str());
        lastImports = imports;
      }
      std::string modules = ModuleReport();
      if (modules != lastModules) {
        Log("Input/overlay modules loaded in the game:%s", modules.c_str());
        lastModules = modules;
      }
      LONG calls = g_totalCalls;
      if (calls != lastCalls) {
        Log("Total controller reads through this DLL so far: %ld", calls);
        lastCalls = calls;
      }
    }
    Sleep(tick < 60 ? 2000 : 30000);
  }
}

}  // namespace

// ----------------------------------------------------------------- exports

extern "C" DWORD WINAPI Proxy_XInputGetState(DWORD user, XINPUT_STATE* state) { return Impl_GetState(user, state); }

extern "C" DWORD WINAPI Proxy_XInputGetStateEx(DWORD user, XINPUT_STATE* state) {
  return Impl_GetStateEx(user, state);
}

extern "C" DWORD WINAPI Proxy_XInputSetState(DWORD user, XINPUT_VIBRATION* vibration) {
  return Impl_SetState(user, vibration);
}

extern "C" DWORD WINAPI Proxy_XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
  return Impl_GetCapabilities(user, flags, caps);
}

extern "C" void WINAPI Proxy_XInputEnable(BOOL enable) { Impl_Enable(enable); }

extern "C" DWORD WINAPI Proxy_XInputGetDSoundAudioDeviceGuids(DWORD user, GUID* render, GUID* capture) {
  return Impl_GetDSoundAudioDeviceGuids(user, render, capture);
}

extern "C" DWORD WINAPI Proxy_XInputGetBatteryInformation(DWORD user, BYTE type, XINPUT_BATTERY_INFORMATION* info) {
  return Impl_GetBatteryInformation(user, type, info);
}

extern "C" DWORD WINAPI Proxy_XInputGetKeystroke(DWORD user, DWORD reserved, PXINPUT_KEYSTROKE ks) {
  return Impl_GetKeystroke(user, reserved, ks);
}

extern "C" DWORD WINAPI Proxy_XInputWaitForGuideButton(DWORD user, DWORD flags, void* listen) {
  return Impl_WaitForGuideButton(user, flags, listen);
}

extern "C" DWORD WINAPI Proxy_XInputCancelGuideButtonWait(DWORD user) { return Impl_CancelGuideButtonWait(user); }

extern "C" DWORD WINAPI Proxy_XInputPowerOffController(DWORD user) { return Impl_PowerOffController(user); }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_self = inst;
    DisableThreadLibraryCalls(inst);
    // The thread only starts running once the loader has finished, so it is
    // safe to load libraries and read the import table from there.
    HANDLE t = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
  }
  return TRUE;
}
