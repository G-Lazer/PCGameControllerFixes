// Test harness: loads build\dinput8.dll (the proxy) by full path, then does what
// a game does - enumerates controllers, opens them, sets the classic joystick
// data format and polls them for a few seconds. Everything shows up in
// build\dinput8_proxy.log, and pressed buttons are also printed here.
//
// Usage: probe.exe [seconds]   (default 0 = just list devices)

#define DIRECTINPUT_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dinput.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef DIDFT_OPTIONAL
#define DIDFT_OPTIONAL 0x80000000
#endif

using PFN_DirectInput8Create =HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

static std::vector<DIDEVICEINSTANCEW> g_devices;

static BOOL CALLBACK OnDevice(LPCDIDEVICEINSTANCEW d, LPVOID) {
  g_devices.push_back(*d);
  return DIENUM_CONTINUE;
}

int wmain(int argc, wchar_t** argv) {
  int seconds = argc > 1 ? _wtoi(argv[1]) : 0;

  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir = exe;
  dir.resize(dir.find_last_of(L'\\') + 1);
  std::wstring proxy = dir + L"dinput8.dll";

  HMODULE mod = LoadLibraryW(proxy.c_str());
  if (!mod) {
    wprintf(L"Could not load %ls (error %lu)\n", proxy.c_str(), GetLastError());
    return 1;
  }
  auto create = reinterpret_cast<PFN_DirectInput8Create>(GetProcAddress(mod, "DirectInput8Create"));
  if (!create) {
    wprintf(L"Proxy has no DirectInput8Create export\n");
    return 1;
  }

  IDirectInput8W* di = nullptr;
  HRESULT hr = create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                      reinterpret_cast<void**>(&di), nullptr);
  if (FAILED(hr)) {
    wprintf(L"DirectInput8Create failed: 0x%08lX\n", hr);
    return 1;
  }

  di->EnumDevices(DI8DEVCLASS_GAMECTRL, OnDevice, nullptr, DIEDFL_ATTACHEDONLY);
  wprintf(L"%zu game controller(s) visible to DirectInput:\n", g_devices.size());
  for (const auto& d : g_devices) wprintf(L"  %ls\n", d.tszProductName);

  // Same layout as the classic c_dfDIJoystick format (DIJOYSTATE).
  std::vector<DIOBJECTDATAFORMAT> objs = {
      {&GUID_XAxis, DIJOFS_X, DIDFT_OPTIONAL | DIDFT_AXIS | DIDFT_ANYINSTANCE, 0},
      {&GUID_YAxis, DIJOFS_Y, DIDFT_OPTIONAL | DIDFT_AXIS | DIDFT_ANYINSTANCE, 0},
      {&GUID_ZAxis, DIJOFS_Z, DIDFT_OPTIONAL | DIDFT_AXIS | DIDFT_ANYINSTANCE, 0},
      {&GUID_RxAxis, DIJOFS_RX, DIDFT_OPTIONAL | DIDFT_AXIS | DIDFT_ANYINSTANCE, 0},
      {&GUID_RyAxis, DIJOFS_RY, DIDFT_OPTIONAL | DIDFT_AXIS | DIDFT_ANYINSTANCE, 0},
      {&GUID_RzAxis, DIJOFS_RZ, DIDFT_OPTIONAL | DIDFT_AXIS | DIDFT_ANYINSTANCE, 0},
      {&GUID_Slider, DIJOFS_SLIDER(0), DIDFT_OPTIONAL | DIDFT_AXIS | DIDFT_ANYINSTANCE, 0},
      {&GUID_Slider, DIJOFS_SLIDER(1), DIDFT_OPTIONAL | DIDFT_AXIS | DIDFT_ANYINSTANCE, 0},
  };
  for (int i = 0; i < 4; ++i)
    objs.push_back({&GUID_POV, static_cast<DWORD>(DIJOFS_POV(i)), DIDFT_OPTIONAL | DIDFT_POV | DIDFT_ANYINSTANCE, 0});
  for (int i = 0; i < 32; ++i)
    objs.push_back({nullptr, static_cast<DWORD>(DIJOFS_BUTTON(i)), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0});
  DIDATAFORMAT fmt = {sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS, sizeof(DIJOYSTATE),
                      static_cast<DWORD>(objs.size()), objs.data()};

  HWND wnd = CreateWindowExW(0, L"STATIC", L"probe", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);

  std::vector<IDirectInputDevice8W*> devs;
  for (const auto& d : g_devices) {
    IDirectInputDevice8W* dev = nullptr;
    if (FAILED(di->CreateDevice(d.guidInstance, &dev, nullptr))) continue;
    DIDEVCAPS caps = {sizeof caps};
    dev->GetCapabilities(&caps);
    dev->SetDataFormat(&fmt);
    dev->SetCooperativeLevel(wnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    dev->Acquire();
    devs.push_back(dev);
  }

  if (seconds > 0) wprintf(L"Polling for %d seconds - press controller buttons now...\n", seconds);
  DWORD end = GetTickCount() + static_cast<DWORD>(seconds) * 1000;
  std::vector<DIJOYSTATE> prev(devs.size());
  while (seconds > 0 && static_cast<LONG>(end - GetTickCount()) > 0) {
    for (size_t i = 0; i < devs.size(); ++i) {
      DIJOYSTATE s = {};
      devs[i]->Poll();
      if (FAILED(devs[i]->GetDeviceState(sizeof s, &s))) {
        devs[i]->Acquire();
        continue;
      }
      for (int b = 0; b < 32; ++b)
        if ((s.rgbButtons[b] & 0x80) && !(prev[i].rgbButtons[b] & 0x80))
          wprintf(L"  %ls: button %d\n", g_devices[i].tszProductName, b + 1);
      prev[i] = s;
    }
    Sleep(10);
  }

  for (auto* dev : devs) {
    dev->Unacquire();
    dev->Release();
  }
  di->Release();
  wprintf(L"Done. See dinput8_proxy.log next to the DLL.\n");
  return 0;
}
