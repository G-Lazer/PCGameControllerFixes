// dinput8.dll proxy for Splinter Cell: Conviction.
//
// The game loads this file instead of Windows' DirectInput because it sits
// next to conviction_game.exe. Every call is passed straight through to the
// real system dinput8.dll, which is loaded only by its full System32 path so
// nothing else can be substituted for it. For game controllers (never the
// keyboard or mouse) the proxy can:
//   - log which devices the game sees and opens, and every button press
//   - remap buttons ([Remap] in dinput8_proxy.ini)
//   - hide devices from the game ([Devices] Hide= in dinput8_proxy.ini)
//   - report controllers under another USB identity ([Spoof] in dinput8_proxy.ini).
//     Conviction reads the vendor/product ID to decide whether a pad is an
//     Xbox 360 controller; anything else gets its Logitech button layout.
//
// It makes no network calls, writes no registry keys and starts no processes.
// The only file it writes is dinput8_proxy.log next to itself.

#define DIRECTINPUT_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dinput.h>
#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr int kMaxButtons = 128;

HMODULE g_self = nullptr;
HMODULE g_real = nullptr;
std::wstring g_dir;  // folder containing this DLL, with trailing backslash
std::once_flag g_initOnce;
std::mutex g_logMutex;
FILE* g_log = nullptr;

bool g_logInput = true;
bool g_remapActive = false;
int g_map[kMaxButtons];            // game button -> physical button (0-based), -1 = always released
std::vector<std::wstring> g_hide;  // lower-case device-name fragments to hide

// Report controllers to the game under another USB identity ([Spoof] in the ini).
bool g_spoof = false;
WORD g_spoofVid = 0;
WORD g_spoofPid = 0;
std::wstring g_spoofName;  // empty = keep the real product name

using PFN_DirectInput8Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using PFN_NoArgs = HRESULT(WINAPI*)();
using PFN_DllGetClassObject = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);

PFN_DirectInput8Create g_realCreate = nullptr;
PFN_NoArgs g_realCanUnloadNow = nullptr;
PFN_DllGetClassObject g_realGetClassObject = nullptr;
PFN_NoArgs g_realRegisterServer = nullptr;
PFN_NoArgs g_realUnregisterServer = nullptr;

// ---------------------------------------------------------------- helpers

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
  if (!s) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr);
  return out;
}

std::wstring Wide(const char* s) {
  if (!s) return {};
  int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
  if (n <= 1) return {};
  std::wstring out(static_cast<size_t>(n - 1), L'\0');
  MultiByteToWideChar(CP_ACP, 0, s, -1, &out[0], n);
  return out;
}

std::wstring Wide(const wchar_t* s) { return s ? s : L""; }
std::string Utf8(const char* s) { return Utf8(Wide(s).c_str()); }

std::wstring Lower(std::wstring s) {
  if (!s.empty()) CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
  return s;
}

std::string GuidStr(const GUID& g) {
  char b[64];
  snprintf(b, sizeof b, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g.Data1, g.Data2,
           g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5],
           g.Data4[6], g.Data4[7]);
  return b;
}

// For HID devices DirectInput packs the USB vendor/product IDs into Data1.
std::string VidPid(const GUID& product) {
  char b[32];
  snprintf(b, sizeof b, "VID_%04X&PID_%04X", LOWORD(product.Data1), HIWORD(product.Data1));
  return b;
}

const char* DevTypeName(DWORD t) {
  switch (GET_DIDEVICE_TYPE(t)) {
    case DI8DEVTYPE_DEVICE: return "Device";
    case DI8DEVTYPE_MOUSE: return "Mouse";
    case DI8DEVTYPE_KEYBOARD: return "Keyboard";
    case DI8DEVTYPE_JOYSTICK: return "Joystick";
    case DI8DEVTYPE_GAMEPAD: return "Gamepad";
    case DI8DEVTYPE_DRIVING: return "Driving";
    case DI8DEVTYPE_FLIGHT: return "Flight";
    case DI8DEVTYPE_1STPERSON: return "FirstPerson";
    case DI8DEVTYPE_DEVICECTRL: return "DeviceCtrl";
    case DI8DEVTYPE_SCREENPOINTER: return "ScreenPointer";
    case DI8DEVTYPE_REMOTE: return "Remote";
    case DI8DEVTYPE_SUPPLEMENTAL: return "Supplemental";
    default: return "Unknown";
  }
}

bool IsGameController(DWORD t) {
  switch (GET_DIDEVICE_TYPE(t)) {
    case DI8DEVTYPE_JOYSTICK:
    case DI8DEVTYPE_GAMEPAD:
    case DI8DEVTYPE_DRIVING:
    case DI8DEVTYPE_FLIGHT:
    case DI8DEVTYPE_1STPERSON:
    case DI8DEVTYPE_SUPPLEMENTAL:
      return true;
    default:
      return false;
  }
}

bool IsHidden(const std::wstring& name) {
  std::wstring lower = Lower(name);
  for (const std::wstring& h : g_hide)
    if (!h.empty() && lower.find(h) != std::wstring::npos) return true;
  return false;
}

std::string PropName(REFGUID prop) {
  // DirectInput passes predefined properties as small integers cast to GUID pointers.
  ULONG_PTR id = reinterpret_cast<ULONG_PTR>(&prop);
  if (id >= 0x10000) return GuidStr(prop);
  switch (id) {
    case 1: return "BUFFERSIZE";
    case 2: return "AXISMODE";
    case 3: return "GRANULARITY";
    case 4: return "RANGE";
    case 5: return "DEADZONE";
    case 6: return "SATURATION";
    case 7: return "FFGAIN";
    case 8: return "FFLOAD";
    case 9: return "AUTOCENTER";
    case 10: return "CALIBRATIONMODE";
    case 11: return "CALIBRATION";
    case 12: return "GUIDANDPATH";
    case 13: return "INSTANCENAME";
    case 14: return "PRODUCTNAME";
    case 15: return "JOYSTICKID";
    case 16: return "GETPORTDISPLAYNAME";
    case 18: return "PHYSICALRANGE";
    case 19: return "LOGICALRANGE";
    case 20: return "KEYNAME";
    case 21: return "CPOINTS";
    case 22: return "APPDATA";
    case 23: return "SCANCODE";
    case 24: return "VIDPID";
    case 25: return "USERNAME";
    case 26: return "TYPENAME";
    default: {
      char b[24];
      snprintf(b, sizeof b, "#%lu", static_cast<unsigned long>(id));
      return b;
    }
  }
}

ULONG_PTR PropId(REFGUID prop) { return reinterpret_cast<ULONG_PTR>(&prop); }

void CopyName(char* dst, const std::wstring& src) {
  WideCharToMultiByte(CP_ACP, 0, src.c_str(), -1, dst, MAX_PATH, nullptr, nullptr);
  dst[MAX_PATH - 1] = '\0';
}
void CopyName(wchar_t* dst, const std::wstring& src) { wcsncpy_s(dst, MAX_PATH, src.c_str(), _TRUNCATE); }

// Rewrite a device description so the game sees the spoofed identity.
// DirectInput stores a HID device's vendor/product IDs in guidProduct.Data1.
template <class DevInst>
void ApplySpoof(DevInst* d) {
  if (!g_spoof || !d || !IsGameController(d->dwDevType)) return;
  d->guidProduct.Data1 = MAKELONG(g_spoofVid, g_spoofPid);
  if (!g_spoofName.empty()) {
    CopyName(d->tszProductName, g_spoofName);
    CopyName(d->tszInstanceName, g_spoofName);
  }
}

// ----------------------------------------------------------- configuration

void LoadConfig() {
  const std::wstring ini = g_dir + L"dinput8_proxy.ini";

  if (GetPrivateProfileIntW(L"General", L"Log", 1, ini.c_str()) != 0)
    g_log = _wfsopen((g_dir + L"dinput8_proxy.log").c_str(), L"w", _SH_DENYWR);
  g_logInput = GetPrivateProfileIntW(L"General", L"LogInput", 1, ini.c_str()) != 0;

  for (int i = 0; i < kMaxButtons; ++i) {
    wchar_t key[16];
    swprintf_s(key, L"Button%d", i + 1);
    int v = static_cast<int>(GetPrivateProfileIntW(L"Remap", key, i + 1, ini.c_str()));
    g_map[i] = (v >= 1 && v <= kMaxButtons) ? v - 1 : -1;
    if (g_map[i] != i) g_remapActive = true;
  }

  wchar_t buf[1024] = {};
  GetPrivateProfileStringW(L"Devices", L"Hide", L"", buf, 1024, ini.c_str());
  std::wstring all = buf;
  size_t start = 0;
  while (start <= all.size()) {
    size_t comma = all.find(L',', start);
    std::wstring part = all.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
    size_t a = part.find_first_not_of(L" \t");
    size_t b = part.find_last_not_of(L" \t");
    if (a != std::wstring::npos) g_hide.push_back(Lower(part.substr(a, b - a + 1)));
    if (comma == std::wstring::npos) break;
    start = comma + 1;
  }

  GetPrivateProfileStringW(L"Spoof", L"VidPid", L"", buf, 1024, ini.c_str());
  unsigned vid = 0, pid = 0;
  if (swscanf_s(buf, L"%x:%x", &vid, &pid) == 2 && vid <= 0xFFFF && pid <= 0xFFFF) {
    g_spoof = true;
    g_spoofVid = static_cast<WORD>(vid);
    g_spoofPid = static_cast<WORD>(pid);
    GetPrivateProfileStringW(L"Spoof", L"ProductName", L"", buf, 1024, ini.c_str());
    g_spoofName = buf;
  }
}

bool Init() {
  std::call_once(g_initOnce, [] {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_self, path, MAX_PATH);
    g_dir = path;
    g_dir.resize(g_dir.find_last_of(L"\\/") + 1);
    LoadConfig();

    GetModuleFileNameW(nullptr, path, MAX_PATH);
    Log("dinput8 proxy loaded into %s", Utf8(path).c_str());

    // Full path only: never search the game folder or PATH for the real DLL.
    wchar_t sys[MAX_PATH] = {};
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring realPath = std::wstring(sys, n) + L"\\dinput8.dll";
    g_real = LoadLibraryExW(realPath.c_str(), nullptr, 0);
    if (g_real == g_self) g_real = nullptr;
    if (!g_real) {
      Log("FATAL: could not load real DirectInput from %s (error %lu)", Utf8(realPath.c_str()).c_str(),
          GetLastError());
      return;
    }
    GetModuleFileNameW(g_real, path, MAX_PATH);
    Log("Real DirectInput: %s", Utf8(path).c_str());

    g_realCreate = reinterpret_cast<PFN_DirectInput8Create>(GetProcAddress(g_real, "DirectInput8Create"));
    g_realCanUnloadNow = reinterpret_cast<PFN_NoArgs>(GetProcAddress(g_real, "DllCanUnloadNow"));
    g_realGetClassObject = reinterpret_cast<PFN_DllGetClassObject>(GetProcAddress(g_real, "DllGetClassObject"));
    g_realRegisterServer = reinterpret_cast<PFN_NoArgs>(GetProcAddress(g_real, "DllRegisterServer"));
    g_realUnregisterServer = reinterpret_cast<PFN_NoArgs>(GetProcAddress(g_real, "DllUnregisterServer"));

    if (g_remapActive) {
      std::string summary;
      for (int i = 0; i < kMaxButtons; ++i) {
        if (g_map[i] == i) continue;
        char b[48];
        if (g_map[i] < 0)
          snprintf(b, sizeof b, " game%d<-none", i + 1);
        else
          snprintf(b, sizeof b, " game%d<-phys%d", i + 1, g_map[i] + 1);
        summary += b;
      }
      Log("Button remap active:%s", summary.c_str());
    } else {
      Log("Button remap: none (buttons passed through unchanged)");
    }
    for (const std::wstring& h : g_hide) Log("Hiding devices whose name contains \"%s\"", Utf8(h.c_str()).c_str());
    if (g_spoof)
      Log("Controllers will be reported as VID_%04X&PID_%04X%s%s%s", g_spoofVid, g_spoofPid,
          g_spoofName.empty() ? "" : " named \"", Utf8(g_spoofName.c_str()).c_str(), g_spoofName.empty() ? "" : "\"");
  });
  return g_realCreate != nullptr;
}

// ------------------------------------------------------------ data format

struct ButtonSlot {
  DWORD ofs;
  int inst;  // 0-based physical button number, -1 if DirectInput won't fill it
};

struct Format {
  DWORD size = 0;
  std::vector<ButtonSlot> buttons;
  std::vector<DWORD> povs;
  std::vector<DWORD> axes;
};

bool IsAxisGuid(const GUID* g) {
  return g && (IsEqualGUID(*g, GUID_XAxis) || IsEqualGUID(*g, GUID_YAxis) || IsEqualGUID(*g, GUID_ZAxis) ||
               IsEqualGUID(*g, GUID_RxAxis) || IsEqualGUID(*g, GUID_RyAxis) || IsEqualGUID(*g, GUID_RzAxis) ||
               IsEqualGUID(*g, GUID_Slider));
}

// Work out which physical button DirectInput will write into each button slot.
// Slots with an explicit instance get that button; "any instance" slots get the
// lowest unclaimed buttons in order, which is how DirectInput fills them.
Format ParseFormat(LPCDIDATAFORMAT f) {
  Format out;
  if (!f || !f->rgodf) return out;
  out.size = f->dwDataSize;
  bool taken[kMaxButtons] = {};
  std::vector<size_t> anyInstance;
  for (DWORD i = 0; i < f->dwNumObjs; ++i) {
    const DIOBJECTDATAFORMAT& o = f->rgodf[i];
    DWORD type = DIDFT_GETTYPE(o.dwType);
    if ((o.pguid && IsEqualGUID(*o.pguid, GUID_POV)) || (!o.pguid && (type & DIDFT_POV))) {
      out.povs.push_back(o.dwOfs);
      continue;
    }
    if (IsAxisGuid(o.pguid) || (!o.pguid && (type & DIDFT_AXIS) && !(type & DIDFT_BUTTON))) {
      out.axes.push_back(o.dwOfs);
      continue;
    }
    bool button = o.pguid ? IsEqualGUID(*o.pguid, GUID_Button) : (type & DIDFT_BUTTON) && !(type & DIDFT_AXIS);
    if (!button) continue;
    WORD inst = static_cast<WORD>(DIDFT_GETINSTANCE(o.dwType));
    out.buttons.push_back({o.dwOfs, -1});
    if (inst != 0xFFFF && inst < kMaxButtons && !taken[inst]) {
      taken[inst] = true;
      out.buttons.back().inst = inst;
    } else {
      anyInstance.push_back(out.buttons.size() - 1);
    }
  }
  int next = 0;
  for (size_t idx : anyInstance) {
    while (next < kMaxButtons && taken[next]) ++next;
    if (next >= kMaxButtons) break;
    taken[next] = true;
    out.buttons[idx].inst = next++;
  }
  return out;
}

// ------------------------------------------------------------------ traits

struct TraitsA {
  using DI = IDirectInput8A;
  using Dev = IDirectInputDevice8A;
  using Str = LPCSTR;
  using DevInst = DIDEVICEINSTANCEA;
  using EnumDevCb = LPDIENUMDEVICESCALLBACKA;
  using SemCb = LPDIENUMDEVICESBYSEMANTICSCBA;
  using ActFmt = LPDIACTIONFORMATA;
  using CfgParams = LPDICONFIGUREDEVICESPARAMSA;
  using ObjInst = DIDEVICEOBJECTINSTANCEA;
  using EnumObjCb = LPDIENUMDEVICEOBJECTSCALLBACKA;
  using EffInfo = LPDIEFFECTINFOA;
  using EnumEffCb = LPDIENUMEFFECTSCALLBACKA;
  using ImgHdr = LPDIDEVICEIMAGEINFOHEADERA;
  static const IID& IidDI() { return IID_IDirectInput8A; }
  static const IID& IidDev() { return IID_IDirectInputDevice8A; }
  static constexpr const char* kSuffix = "A";
};

struct TraitsW {
  using DI = IDirectInput8W;
  using Dev = IDirectInputDevice8W;
  using Str = LPCWSTR;
  using DevInst = DIDEVICEINSTANCEW;
  using EnumDevCb = LPDIENUMDEVICESCALLBACKW;
  using SemCb = LPDIENUMDEVICESBYSEMANTICSCBW;
  using ActFmt = LPDIACTIONFORMATW;
  using CfgParams = LPDICONFIGUREDEVICESPARAMSW;
  using ObjInst = DIDEVICEOBJECTINSTANCEW;
  using EnumObjCb = LPDIENUMDEVICEOBJECTSCALLBACKW;
  using EffInfo = LPDIEFFECTINFOW;
  using EnumEffCb = LPDIENUMEFFECTSCALLBACKW;
  using ImgHdr = LPDIDEVICEIMAGEINFOHEADERW;
  static const IID& IidDI() { return IID_IDirectInput8W; }
  static const IID& IidDev() { return IID_IDirectInputDevice8W; }
  static constexpr const char* kSuffix = "W";
};

// ----------------------------------------------------------- device proxy

template <class T>
class DeviceProxy final : public T::Dev {
 public:
  DeviceProxy(typename T::Dev* real, std::string name) : real_(real), name_(std::move(name)) {
    memset(prevDown_, 0, sizeof prevDown_);
    for (DWORD& p : prevPov_) p = 0xFFFFFFFF;
  }

  // IUnknown. The reference count lives on the real device.
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppv) override {
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, T::IidDev())) {
      AddRef();
      *ppv = this;
      return S_OK;
    }
    Log("[%s] QueryInterface(%s) passed through unwrapped", name_.c_str(), GuidStr(riid).c_str());
    return real_->QueryInterface(riid, ppv);
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return real_->AddRef(); }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG n = real_->Release();
    if (n == 0) {
      Log("[%s] released", name_.c_str());
      delete this;
    }
    return n;
  }

  HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS caps) override {
    HRESULT hr = real_->GetCapabilities(caps);
    if (SUCCEEDED(hr) && caps && !loggedCaps_) {
      loggedCaps_ = true;
      Log("[%s] GetCapabilities: %lu buttons, %lu axes, %lu POVs, type %s", name_.c_str(), caps->dwButtons,
          caps->dwAxes, caps->dwPOVs, DevTypeName(caps->dwDevType));
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE EnumObjects(typename T::EnumObjCb cb, LPVOID ref, DWORD flags) override {
    if (!cb || loggedObjects_) return real_->EnumObjects(cb, ref, flags);
    loggedObjects_ = true;
    Log("[%s] EnumObjects(flags=0x%lX):", name_.c_str(), flags);
    ObjCtx ctx{cb, ref};
    return real_->EnumObjects(&ObjThunk, &ctx, flags);
  }

  HRESULT STDMETHODCALLTYPE GetProperty(REFGUID prop, LPDIPROPHEADER hdr) override {
    HRESULT hr = real_->GetProperty(prop, hdr);
    ULONG_PTR id = PropId(prop);
    if (SUCCEEDED(hr) && hdr && id == PropId(DIPROP_VIDPID)) {
      DWORD& v = reinterpret_cast<LPDIPROPDWORD>(hdr)->dwData;
      if (g_spoof) {
        Log("[%s] game read VIDPID: real VID_%04X&PID_%04X, reported VID_%04X&PID_%04X", name_.c_str(), LOWORD(v),
            HIWORD(v), g_spoofVid, g_spoofPid);
        v = MAKELONG(g_spoofVid, g_spoofPid);
      } else {
        Log("[%s] game read VIDPID: VID_%04X&PID_%04X", name_.c_str(), LOWORD(v), HIWORD(v));
      }
    } else if (SUCCEEDED(hr) && hdr && (id == PropId(DIPROP_PRODUCTNAME) || id == PropId(DIPROP_INSTANCENAME))) {
      auto* s = reinterpret_cast<LPDIPROPSTRING>(hdr);
      if (g_spoof && !g_spoofName.empty()) wcsncpy_s(s->wsz, MAX_PATH, g_spoofName.c_str(), _TRUNCATE);
      Log("[%s] game read %s: \"%s\"", name_.c_str(), PropName(prop).c_str(), Utf8(s->wsz).c_str());
    } else if (id != PropId(DIPROP_RANGE)) {
      Log("[%s] GetProperty(%s) = 0x%08lX", name_.c_str(), PropName(prop).c_str(), hr);
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE SetProperty(REFGUID prop, LPCDIPROPHEADER hdr) override {
    if (PropId(prop) == PropId(DIPROP_RANGE)) ranges_.clear();
    HRESULT hr = real_->SetProperty(prop, hdr);
    if (setPropLogs_ < 40) {
      ++setPropLogs_;
      Log("[%s] SetProperty(%s, obj=%lu how=%lu) = 0x%08lX", name_.c_str(), PropName(prop).c_str(),
          hdr ? hdr->dwObj : 0, hdr ? hdr->dwHow : 0, hr);
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE Acquire() override {
    HRESULT hr = real_->Acquire();
    if (!loggedAcquire_ && SUCCEEDED(hr)) {
      loggedAcquire_ = true;
      Log("[%s] acquired by game", name_.c_str());
    }
    return hr;
  }
  HRESULT STDMETHODCALLTYPE Unacquire() override { return real_->Unacquire(); }

  HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD cb, LPVOID data) override {
    HRESULT hr = real_->GetDeviceState(cb, data);
    if (SUCCEEDED(hr) && data) {
      if (!loggedPoll_) {
        loggedPoll_ = true;
        Log("[%s] game is polling GetDeviceState (%lu bytes)", name_.c_str(), cb);
      }
      OnState(static_cast<BYTE*>(data), cb);
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD cbObj, LPDIDEVICEOBJECTDATA rg, LPDWORD inOut, DWORD flags) override {
    HRESULT hr = real_->GetDeviceData(cbObj, rg, inOut, flags);
    if (SUCCEEDED(hr) && rg && inOut && cbObj >= 2 * sizeof(DWORD)) {
      if (!loggedBuffered_ && *inOut) {
        loggedBuffered_ = true;
        Log("[%s] game is reading buffered GetDeviceData", name_.c_str());
      }
      BYTE* p = reinterpret_cast<BYTE*>(rg);
      for (DWORD i = 0; i < *inOut; ++i) OnEvent(reinterpret_cast<LPDIDEVICEOBJECTDATA>(p + i * cbObj));
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT f) override {
    HRESULT hr = real_->SetDataFormat(f);
    if (SUCCEEDED(hr)) {
      fmt_ = ParseFormat(f);
      ranges_.clear();
      std::string slots;
      for (const ButtonSlot& s : fmt_.buttons) {
        if (s.inst < 0 || s.inst >= 16) continue;
        char b[24];
        snprintf(b, sizeof b, " b%d@%lu", s.inst + 1, s.ofs);
        slots += b;
      }
      Log("[%s] SetDataFormat: %lu bytes, %zu button slots, %zu axes, %zu POVs;%s", name_.c_str(), fmt_.size,
          fmt_.buttons.size(), fmt_.axes.size(), fmt_.povs.size(), slots.c_str());
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE h) override { return real_->SetEventNotification(h); }
  HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND w, DWORD flags) override {
    Log("[%s] SetCooperativeLevel(flags=0x%lX)", name_.c_str(), flags);
    return real_->SetCooperativeLevel(w, flags);
  }
  HRESULT STDMETHODCALLTYPE GetObjectInfo(typename T::ObjInst* oi, DWORD obj, DWORD how) override {
    return real_->GetObjectInfo(oi, obj, how);
  }
  HRESULT STDMETHODCALLTYPE GetDeviceInfo(typename T::DevInst* di) override {
    HRESULT hr = real_->GetDeviceInfo(di);
    if (SUCCEEDED(hr)) ApplySpoof(di);
    return hr;
  }
  HRESULT STDMETHODCALLTYPE RunControlPanel(HWND w, DWORD flags) override { return real_->RunControlPanel(w, flags); }
  HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE h, DWORD v, REFGUID g) override { return real_->Initialize(h, v, g); }
  HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT* out, LPUNKNOWN outer) override {
    return real_->CreateEffect(g, e, out, outer);
  }
  HRESULT STDMETHODCALLTYPE EnumEffects(typename T::EnumEffCb cb, LPVOID ref, DWORD type) override {
    return real_->EnumEffects(cb, ref, type);
  }
  HRESULT STDMETHODCALLTYPE GetEffectInfo(typename T::EffInfo info, REFGUID g) override {
    return real_->GetEffectInfo(info, g);
  }
  HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD s) override { return real_->GetForceFeedbackState(s); }
  HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD c) override { return real_->SendForceFeedbackCommand(c); }
  HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ref,
                                                     DWORD flags) override {
    return real_->EnumCreatedEffectObjects(cb, ref, flags);
  }
  HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE e) override { return real_->Escape(e); }
  HRESULT STDMETHODCALLTYPE Poll() override { return real_->Poll(); }
  HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD cbObj, LPCDIDEVICEOBJECTDATA rg, LPDWORD inOut, DWORD flags) override {
    return real_->SendDeviceData(cbObj, rg, inOut, flags);
  }
  HRESULT STDMETHODCALLTYPE EnumEffectsInFile(typename T::Str file, LPDIENUMEFFECTSINFILECALLBACK cb, LPVOID ref,
                                              DWORD flags) override {
    return real_->EnumEffectsInFile(file, cb, ref, flags);
  }
  HRESULT STDMETHODCALLTYPE WriteEffectToFile(typename T::Str file, DWORD n, LPDIFILEEFFECT e, DWORD flags) override {
    return real_->WriteEffectToFile(file, n, e, flags);
  }
  HRESULT STDMETHODCALLTYPE BuildActionMap(typename T::ActFmt af, typename T::Str user, DWORD flags) override {
    Log("[%s] BuildActionMap called (action mapping is passed through unchanged)", name_.c_str());
    return real_->BuildActionMap(af, user, flags);
  }
  HRESULT STDMETHODCALLTYPE SetActionMap(typename T::ActFmt af, typename T::Str user, DWORD flags) override {
    return real_->SetActionMap(af, user, flags);
  }
  HRESULT STDMETHODCALLTYPE GetImageInfo(typename T::ImgHdr h) override { return real_->GetImageInfo(h); }

 private:
  struct ObjCtx {
    typename T::EnumObjCb cb;
    LPVOID ref;
  };

  static BOOL CALLBACK ObjThunk(const typename T::ObjInst* o, LPVOID p) {
    auto* ctx = static_cast<ObjCtx*>(p);
    Log("    object \"%s\" type=0x%08lX instance=%lu ofs=%lu", Utf8(o->tszName).c_str(), o->dwType,
        static_cast<unsigned long>(DIDFT_GETINSTANCE(o->dwType)), o->dwOfs);
    return ctx->cb(o, ctx->ref);
  }

  int GameButtonFor(int phys) const {
    for (int q = 0; q < kMaxButtons; ++q)
      if (g_map[q] == phys) return q;
    return -1;
  }

  const ButtonSlot* SlotForInst(int inst) const {
    for (const ButtonSlot& s : fmt_.buttons)
      if (s.inst == inst) return &s;
    return nullptr;
  }

  void LogButton(int phys, bool down, const char* how) {
    int game = g_remapActive ? GameButtonFor(phys) : phys;
    if (game == phys)
      Log("[%s] button %d %s%s", name_.c_str(), phys + 1, down ? "DOWN" : "up", how);
    else if (game < 0)
      Log("[%s] button %d %s%s (not given to game)", name_.c_str(), phys + 1, down ? "DOWN" : "up", how);
    else
      Log("[%s] button %d %s%s -> game sees button %d", name_.c_str(), phys + 1, down ? "DOWN" : "up", how, game + 1);
  }

  void OnState(BYTE* d, DWORD cb) {
    BYTE phys[kMaxButtons] = {};
    for (const ButtonSlot& s : fmt_.buttons)
      if (s.inst >= 0 && s.ofs < cb) phys[s.inst] = d[s.ofs];

    if (g_logInput) {
      for (const ButtonSlot& s : fmt_.buttons) {
        if (s.inst < 0 || s.ofs >= cb) continue;
        bool down = (phys[s.inst] & 0x80) != 0;
        if (down != prevDown_[s.inst]) {
          prevDown_[s.inst] = down;
          LogButton(s.inst, down, "");
        }
      }
      for (size_t i = 0; i < fmt_.povs.size() && i < 4; ++i) {
        if (fmt_.povs[i] + sizeof(DWORD) > cb) continue;
        DWORD v;
        memcpy(&v, d + fmt_.povs[i], sizeof v);
        DWORD norm = LOWORD(v) == 0xFFFF ? 0xFFFFFFFF : v;
        if (norm != prevPov_[i]) {
          prevPov_[i] = norm;
          if (norm == 0xFFFFFFFF)
            Log("[%s] D-pad/POV %zu centered", name_.c_str(), i + 1);
          else
            Log("[%s] D-pad/POV %zu at %lu degrees", name_.c_str(), i + 1, norm / 100);
        }
      }
      LogAxes(d, cb);
    }

    if (g_remapActive) {
      for (const ButtonSlot& s : fmt_.buttons) {
        if (s.inst < 0 || s.ofs >= cb) continue;
        int src = g_map[s.inst];
        d[s.ofs] = (src >= 0 && src < kMaxButtons) ? phys[src] : 0;
      }
    }
  }

  struct AxisRange {
    DWORD ofs;
    LONG min, max, last;
    bool seen;
  };

  void LogAxes(const BYTE* d, DWORD cb) {
    if (ranges_.empty()) {
      for (DWORD ofs : fmt_.axes) {
        DIPROPRANGE r = {};
        r.diph.dwSize = sizeof r;
        r.diph.dwHeaderSize = sizeof r.diph;
        r.diph.dwObj = ofs;
        r.diph.dwHow = DIPH_BYOFFSET;
        if (FAILED(real_->GetProperty(DIPROP_RANGE, &r.diph)) || r.lMax <= r.lMin) {
          r.lMin = 0;
          r.lMax = 65535;
        }
        ranges_.push_back({ofs, r.lMin, r.lMax, 0, false});
      }
    }
    for (AxisRange& a : ranges_) {
      if (a.ofs + sizeof(LONG) > cb) continue;
      LONG v;
      memcpy(&v, d + a.ofs, sizeof v);
      LONG span = a.max - a.min;
      if (!a.seen) {
        a.seen = true;
        a.last = v;
        continue;
      }
      LONG delta = v > a.last ? v - a.last : a.last - v;
      if (span > 0 && delta * 4 >= span) {
        a.last = v;
        Log("[%s] axis @%lu moved to %ld%% of range", name_.c_str(), a.ofs,
            static_cast<long>((static_cast<long long>(v - a.min) * 100) / span));
      }
    }
  }

  void OnEvent(LPDIDEVICEOBJECTDATA e) {
    const ButtonSlot* slot = nullptr;
    for (const ButtonSlot& s : fmt_.buttons)
      if (s.ofs == e->dwOfs) slot = &s;
    if (!slot || slot->inst < 0) return;
    int phys = slot->inst;
    if (g_logInput) LogButton(phys, (e->dwData & 0x80) != 0, " (buffered)");
    if (!g_remapActive) return;
    int game = GameButtonFor(phys);
    const ButtonSlot* target = game >= 0 ? SlotForInst(game) : nullptr;
    if (target) e->dwOfs = target->ofs;
  }

  typename T::Dev* real_;
  std::string name_;
  Format fmt_;
  std::vector<AxisRange> ranges_;
  bool prevDown_[kMaxButtons];
  DWORD prevPov_[4];
  bool loggedCaps_ = false;
  bool loggedObjects_ = false;
  bool loggedAcquire_ = false;
  bool loggedPoll_ = false;
  bool loggedBuffered_ = false;
  int setPropLogs_ = 0;
};

// ------------------------------------------------------------ input proxy

template <class T>
class InputProxy final : public T::DI {
 public:
  explicit InputProxy(typename T::DI* real) : real_(real) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppv) override {
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, T::IidDI())) {
      AddRef();
      *ppv = this;
      return S_OK;
    }
    Log("IDirectInput8%s::QueryInterface(%s) passed through unwrapped", T::kSuffix, GuidStr(riid).c_str());
    return real_->QueryInterface(riid, ppv);
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return real_->AddRef(); }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG n = real_->Release();
    if (n == 0) delete this;
    return n;
  }

  HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID guid, typename T::Dev** out, LPUNKNOWN outer) override {
    HRESULT hr = real_->CreateDevice(guid, out, outer);
    if (FAILED(hr) || !out || !*out) {
      Log("CreateDevice(%s) failed: 0x%08lX", GuidStr(guid).c_str(), hr);
      return hr;
    }
    typename T::DevInst info = {};
    info.dwSize = sizeof info;
    if (FAILED((*out)->GetDeviceInfo(&info))) {
      Log("CreateDevice(%s): no device info, passed through", GuidStr(guid).c_str());
      return hr;
    }
    std::string name = Utf8(info.tszProductName);
    if (!IsGameController(info.dwDevType)) {
      Log("Game opened %s \"%s\" (passed through untouched)", DevTypeName(info.dwDevType), name.c_str());
      return hr;
    }
    Log("Game opened controller \"%s\" type=%s %s instance=%s -> wrapped", name.c_str(),
        DevTypeName(info.dwDevType), VidPid(info.guidProduct).c_str(), GuidStr(guid).c_str());
    *out = new DeviceProxy<T>(*out, name);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE EnumDevices(DWORD type, typename T::EnumDevCb cb, LPVOID ref, DWORD flags) override {
    if (!cb) return real_->EnumDevices(type, cb, ref, flags);
    Log("Game enumerates devices (class/type=0x%lX, flags=0x%lX):", type, flags);
    EnumCtx ctx{cb, ref, 0};
    HRESULT hr = real_->EnumDevices(type, &EnumThunk, &ctx, flags);
    Log("  ...%d device(s) listed", ctx.n);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID g) override { return real_->GetDeviceStatus(g); }
  HRESULT STDMETHODCALLTYPE RunControlPanel(HWND w, DWORD flags) override { return real_->RunControlPanel(w, flags); }
  HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE h, DWORD v) override { return real_->Initialize(h, v); }
  HRESULT STDMETHODCALLTYPE FindDevice(REFGUID cls, typename T::Str name, LPGUID out) override {
    return real_->FindDevice(cls, name, out);
  }
  HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(typename T::Str user, typename T::ActFmt af, typename T::SemCb cb,
                                                   LPVOID ref, DWORD flags) override {
    Log("Game uses EnumDevicesBySemantics (action mapping): devices from it are NOT wrapped");
    return real_->EnumDevicesBySemantics(user, af, cb, ref, flags);
  }
  HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK cb, typename T::CfgParams params, DWORD flags,
                                             LPVOID ref) override {
    return real_->ConfigureDevices(cb, params, flags, ref);
  }

 private:
  struct EnumCtx {
    typename T::EnumDevCb cb;
    LPVOID ref;
    int n;
  };

  static BOOL CALLBACK EnumThunk(const typename T::DevInst* d, LPVOID p) {
    auto* ctx = static_cast<EnumCtx*>(p);
    std::wstring product = Wide(d->tszProductName);
    std::wstring instance = Wide(d->tszInstanceName);
    bool hide = IsHidden(product) || IsHidden(instance);
    Log("  %s \"%s\" type=%s(0x%08lX) %s%s", hide ? "HIDDEN" : "device", Utf8(product.c_str()).c_str(),
        DevTypeName(d->dwDevType), d->dwDevType, VidPid(d->guidProduct).c_str(),
        IsGameController(d->dwDevType) ? " [controller]" : "");
    if (hide) return DIENUM_CONTINUE;
    ++ctx->n;
    typename T::DevInst shown = *d;
    ApplySpoof(&shown);
    return ctx->cb(&shown, ctx->ref);
  }

  typename T::DI* real_;
};

}  // namespace

// ----------------------------------------------------------------- exports

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE hinst, DWORD version, REFIID riid, LPVOID* out,
                                                   LPUNKNOWN outer) {
  if (!Init()) return E_FAIL;
  HRESULT hr = g_realCreate(hinst, version, riid, out, outer);
  if (FAILED(hr) || !out || !*out) {
    Log("DirectInput8Create(version 0x%04lX) failed: 0x%08lX", version, hr);
    return hr;
  }
  if (IsEqualIID(riid, IID_IDirectInput8A)) {
    *out = static_cast<IDirectInput8A*>(new InputProxy<TraitsA>(static_cast<IDirectInput8A*>(*out)));
    Log("DirectInput8Create(version 0x%04lX, ANSI) -> wrapped", version);
  } else if (IsEqualIID(riid, IID_IDirectInput8W)) {
    *out = static_cast<IDirectInput8W*>(new InputProxy<TraitsW>(static_cast<IDirectInput8W*>(*out)));
    Log("DirectInput8Create(version 0x%04lX, Unicode) -> wrapped", version);
  } else {
    Log("DirectInput8Create for unexpected interface %s, passed through", GuidStr(riid).c_str());
  }
  return hr;
}

extern "C" HRESULT WINAPI Proxy_DllCanUnloadNow() {
  return Init() && g_realCanUnloadNow ? g_realCanUnloadNow() : S_FALSE;
}

extern "C" HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
  return Init() && g_realGetClassObject ? g_realGetClassObject(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT WINAPI Proxy_DllRegisterServer() {
  return Init() && g_realRegisterServer ? g_realRegisterServer() : E_FAIL;
}

extern "C" HRESULT WINAPI Proxy_DllUnregisterServer() {
  return Init() && g_realUnregisterServer ? g_realUnregisterServer() : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_self = inst;
    DisableThreadLibraryCalls(inst);
  }
  return TRUE;
}
