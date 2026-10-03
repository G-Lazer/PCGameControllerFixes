// Reproduces what Steam's gameoverlayrenderer.dll does to the game, then
// checks the proxy still inverts the right stick.
//
// This exe imports XInputGetState from xinput1_3.dll the same way the game
// does (through its import table), so build\xinput1_3.dll - the proxy - is
// loaded from this folder. The test then patches the start of the proxy's
// exported XInputGetState with a jump to a fake "overlay" function that reads
// Windows' real XInput directly, exactly the shape of patch Steam applied.
//
// Pass: calls through the import table still come back inverted.
// Exit code 0 = pass.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>

#include <cstdio>
#include <cstring>

using PFN_GetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
static PFN_GetState g_systemGetState;
static volatile LONG g_fakeHookCalls;

// Stands in for Steam's hook: reads the controller itself, never calls the proxy.
static DWORD WINAPI FakeOverlayGetState(DWORD user, XINPUT_STATE* s) {
  InterlockedIncrement(&g_fakeHookCalls);
  return g_systemGetState(user, s);
}

static bool PatchWithJump(void* target, const void* dest) {
  BYTE patch[5] = {0xE9};
  LONG rel = static_cast<LONG>(reinterpret_cast<const BYTE*>(dest) - (static_cast<BYTE*>(target) + 5));
  memcpy(patch + 1, &rel, sizeof rel);
  DWORD old;
  if (!VirtualProtect(target, sizeof patch, PAGE_EXECUTE_READWRITE, &old)) return false;
  memcpy(target, patch, sizeof patch);
  VirtualProtect(target, sizeof patch, old, &old);
  FlushInstructionCache(GetCurrentProcess(), target, sizeof patch);
  return true;
}

static SHORT Flip(SHORT v) { return v == -32768 ? 32767 : static_cast<SHORT>(-v); }

// Read through the import table and the real DLL until both see the same packet.
static bool ReadPair(XINPUT_STATE& viaGame, XINPUT_STATE& viaSystem) {
  for (int i = 0; i < 200; ++i) {
    if (XInputGetState(0, &viaGame) != ERROR_SUCCESS || g_systemGetState(0, &viaSystem) != ERROR_SUCCESS)
      return false;
    if (viaGame.dwPacketNumber == viaSystem.dwPacketNumber) return true;
    Sleep(5);
  }
  return false;
}

int main() {
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  wcscat_s(sys, L"\\xinput1_3.dll");
  HMODULE systemDll = LoadLibraryW(sys);
  HMODULE proxy = GetModuleHandleW(L"xinput1_3.dll");
  wchar_t proxyPath[MAX_PATH] = {};
  GetModuleFileNameW(proxy, proxyPath, MAX_PATH);
  printf("proxy loaded from: %ls\n", proxyPath);
  if (!systemDll || !proxy || proxy == systemDll) {
    printf("FAIL: could not load both the proxy and the system DLL\n");
    return 1;
  }
  g_systemGetState = reinterpret_cast<PFN_GetState>(GetProcAddress(systemDll, "XInputGetState"));

  // Give the proxy's background thread time to reroute this exe's imports.
  Sleep(1500);

  void* exported = reinterpret_cast<void*>(GetProcAddress(proxy, "XInputGetState"));
  if (!PatchWithJump(exported, reinterpret_cast<const void*>(&FakeOverlayGetState))) {
    printf("FAIL: could not patch the export\n");
    return 1;
  }
  printf("patched the proxy's exported XInputGetState the way Steam does\n");

  XINPUT_STATE g = {}, s = {};
  int checked = 0, inverted = 0;
  for (int i = 0; i < 100; ++i) {
    if (!ReadPair(g, s)) {
      printf("FAIL: no controller on slot 0, or reads never lined up\n");
      return 2;
    }
    ++checked;
    if (g.Gamepad.sThumbRY == Flip(s.Gamepad.sThumbRY) && g.Gamepad.sThumbLY == s.Gamepad.sThumbLY &&
        g.Gamepad.wButtons == s.Gamepad.wButtons)
      ++inverted;
    Sleep(5);
  }

  // Calling the patched export directly should now hit the fake overlay instead.
  XINPUT_STATE d = {};
  reinterpret_cast<PFN_GetState>(exported)(0, &d);

  printf("game-style reads: %d checked, %d with right stick Y inverted (example: real %d -> game got %d)\n", checked,
         inverted, s.Gamepad.sThumbRY, g.Gamepad.sThumbRY);
  printf("fake overlay hook was called %ld time(s) by direct calls to the patched export\n", g_fakeHookCalls);
  bool pass = checked > 0 && inverted == checked && g_fakeHookCalls > 0;
  printf(pass ? "PASS\n" : "FAIL\n");
  return pass ? 0 : 3;
}
