// Test harness: loads build\xinput1_3.dll (the proxy) and Windows' real XInput
// side by side, reads controller 0 through both and checks the proxy only
// flipped the right stick's Y axis. Also checks the hidden ordinal exports.
//
// Usage: probe.exe [seconds]   (default 2; move the right stick to see values change)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>

#include <cstdio>
#include <cstdlib>
#include <string>

using PFN_GetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

int wmain(int argc, wchar_t** argv) {
  int seconds = argc > 1 ? _wtoi(argv[1]) : 2;

  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring dir = buf;
  dir.resize(dir.find_last_of(L'\\') + 1);
  GetSystemDirectoryW(buf, MAX_PATH);
  std::wstring sys = std::wstring(buf) + L"\\xinput1_3.dll";

  HMODULE proxy = LoadLibraryW((dir + L"xinput1_3.dll").c_str());
  HMODULE real = LoadLibraryW(sys.c_str());
  if (!proxy || !real || proxy == real) {
    wprintf(L"Load failed (proxy=%p real=%p)\n", proxy, real);
    return 1;
  }

  const char* names[] = {"XInputGetState", "XInputSetState", "XInputGetCapabilities", "XInputEnable",
                         "XInputGetDSoundAudioDeviceGuids", "XInputGetBatteryInformation", "XInputGetKeystroke"};
  for (const char* n : names) wprintf(L"  export %hs: %ls\n", n, GetProcAddress(proxy, n) ? L"ok" : L"MISSING");
  for (int ord = 100; ord <= 103; ++ord)
    wprintf(L"  export ordinal %d: %ls\n", ord, GetProcAddress(proxy, MAKEINTRESOURCEA(ord)) ? L"ok" : L"MISSING");

  auto viaProxy = reinterpret_cast<PFN_GetState>(GetProcAddress(proxy, "XInputGetState"));
  auto viaProxyEx = reinterpret_cast<PFN_GetState>(GetProcAddress(proxy, MAKEINTRESOURCEA(100)));
  auto viaReal = reinterpret_cast<PFN_GetState>(GetProcAddress(real, "XInputGetState"));

  int samples = 0, flipped = 0, otherChanged = 0;
  DWORD end = GetTickCount() + static_cast<DWORD>(seconds) * 1000;
  while (static_cast<LONG>(end - GetTickCount()) > 0) {
    XINPUT_STATE r = {}, p = {}, px = {};
    if (viaReal(0, &r) != ERROR_SUCCESS || viaProxy(0, &p) != ERROR_SUCCESS || viaProxyEx(0, &px) != ERROR_SUCCESS) {
      wprintf(L"No controller on slot 0\n");
      return 2;
    }
    // Same packet means the two reads saw the same controller state.
    if (r.dwPacketNumber == p.dwPacketNumber) {
      ++samples;
      SHORT expect = r.Gamepad.sThumbRY == -32768 ? 32767 : static_cast<SHORT>(-r.Gamepad.sThumbRY);
      if (p.Gamepad.sThumbRY == expect && px.Gamepad.sThumbRY == expect) ++flipped;
      if (p.Gamepad.sThumbLX != r.Gamepad.sThumbLX || p.Gamepad.sThumbLY != r.Gamepad.sThumbLY ||
          p.Gamepad.sThumbRX != r.Gamepad.sThumbRX || p.Gamepad.wButtons != r.Gamepad.wButtons ||
          p.Gamepad.bLeftTrigger != r.Gamepad.bLeftTrigger || p.Gamepad.bRightTrigger != r.Gamepad.bRightTrigger)
        ++otherChanged;
      if (samples % 50 == 1)
        wprintf(L"  right stick Y: real %6d  proxy %6d  proxy(ord100) %6d\n", r.Gamepad.sThumbRY, p.Gamepad.sThumbRY,
                px.Gamepad.sThumbRY);
    }
    Sleep(10);
  }
  wprintf(L"%d matched samples: %d with right-stick Y flipped, %d with any other value changed\n", samples, flipped,
          otherChanged);
  return (samples > 0 && flipped == samples && otherChanged == 0) ? 0 : 3;
}
