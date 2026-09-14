// Explicit four-second public read-only probe. No camera acquisition or writes.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

int main() {
  const auto module = LoadLibraryExW(L"C:/Program Files/Little Navmap/SimConnect_msfs_2024.dll", nullptr,
                                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!module) {
    std::printf("{\"error\":\"load_library\",\"win32\":%lu}\n", GetLastError());
    return 1;
  }
  using Open = HRESULT(WINAPI*)(HANDLE*, const char*, HWND, DWORD, HANDLE, DWORD);
  using Close = HRESULT(WINAPI*)(HANDLE);
  using Define = HRESULT(WINAPI*)(HANDLE, DWORD, const char*, const char*, DWORD, float, DWORD);
  using Request = HRESULT(WINAPI*)(HANDLE, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
  using Dispatch = HRESULT(WINAPI*)(HANDLE, void**, DWORD*);
  const auto open = reinterpret_cast<Open>(GetProcAddress(module, "SimConnect_Open"));
  const auto close = reinterpret_cast<Close>(GetProcAddress(module, "SimConnect_Close"));
  const auto define = reinterpret_cast<Define>(GetProcAddress(module, "SimConnect_AddToDataDefinition"));
  const auto request = reinterpret_cast<Request>(GetProcAddress(module, "SimConnect_RequestDataOnSimObject"));
  const auto dispatch = reinterpret_cast<Dispatch>(GetProcAddress(module, "SimConnect_GetNextDispatch"));
  if (!open || !close || !define || !request || !dispatch) {
    std::puts("{\"error\":\"missing_export\"}");
    FreeLibrary(module);
    return 1;
  }
  HANDLE session = nullptr;
  const auto opened = open(&session, "Taxi Cam bounded public lighting sample", nullptr, 0, nullptr, 0);
  if (FAILED(opened)) {
    std::printf("{\"error\":\"open\",\"hresult\":%lu}\n", static_cast<unsigned long>(opened));
    FreeLibrary(module);
    return 1;
  }
  const char* names[]{"AMBIENT LIGHT SENSOR", "GLASSCOCKPIT AUTOMATIC BRIGHTNESS"};
  std::array<bool, 2> enabled{};
  std::array<unsigned, 2> samples{};
  for (unsigned i = 0; i < 2; ++i)
    enabled[i] = SUCCEEDED(define(session, 41 + i, names[i], "number", 4, 0, 0xffffffffu));
  const auto start = GetTickCount64();
  std::uint64_t previous = 0;
  while (GetTickCount64() - start < 4000) {
    const auto now = GetTickCount64();
    if (now - previous >= 500) {
      previous = now;
      for (unsigned i = 0; i < 2; ++i)
        if (enabled[i])
          request(session, 41 + i, 41 + i, 0, 1, 0, 0, 0, 0);
    }
    for (unsigned n = 0; n < 64; ++n) {
      void* packet = nullptr;
      DWORD bytes = 0;
      if (FAILED(dispatch(session, &packet, &bytes)) || !packet)
        break;
      if (bytes < 12 || bytes > 4096)
        continue;
      std::array<DWORD, 10> header{};
      std::memcpy(header.data(), packet, bytes >= 40 ? 40 : 12);
      if (header[2] == 8 && bytes == 48 && header[0] == 48 && header[3] >= 41 && header[3] <= 42 && header[5] == header[3] &&
          header[6] == 0 && header[9] == 1) {
        double value = 0;
        std::memcpy(&value, static_cast<const unsigned char*>(packet) + 40, 8);
        const auto index = header[3] - 41;
        if (std::isfinite(value)) {
          ++samples[index];
          std::printf("{\"variable\":\"%s\",\"value\":%.17g,\"elapsed_ms\":%llu}\n", names[index], value,
                      static_cast<unsigned long long>(GetTickCount64() - start));
        }
      } else if (header[2] == 1 && bytes >= 24) {
        std::array<DWORD, 6> exception{};
        std::memcpy(exception.data(), packet, sizeof(exception));
        std::printf("{\"exception\":%lu,\"send_id\":%lu}\n", exception[3], exception[4]);
      }
    }
    Sleep(5);
  }
  close(session);
  FreeLibrary(module);
  std::printf("{\"complete\":true,\"ambient_samples\":%u,\"brightness_samples\":%u}\n", samples[0], samples[1]);
  return samples[1] == 0 ? 1 : 0;
}
