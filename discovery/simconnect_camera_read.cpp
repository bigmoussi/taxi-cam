// Bounded public CameraGet diagnostic. No CameraSet or private engine calls.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
using Open = HRESULT(WINAPI*)(HANDLE*, const char*, HWND, DWORD, HANDLE, DWORD);
using Close = HRESULT(WINAPI*)(HANDLE);
using CameraName = HRESULT(WINAPI*)(HANDLE, const char*);
using CameraGet = HRESULT(WINAPI*)(HANDLE, DWORD);
using Dispatch = HRESULT(WINAPI*)(HANDLE, void**, DWORD*);
int main(int argc, char** argv) {
  const bool acquire = argc == 2 && std::strcmp(argv[1], "--acquire") == 0;
  if (argc != 1 && !acquire) return 2;
  auto dll = LoadLibraryExW(L"C:/XboxGames/Microsoft Flight Simulator 2024/Content/SimConnect_internal.dll", nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!dll) { std::printf("{\"load_error\":%lu}\n", GetLastError()); return 1; }
  auto open = reinterpret_cast<Open>(GetProcAddress(dll, "SimConnect_Open"));
  auto close = reinterpret_cast<Close>(GetProcAddress(dll, "SimConnect_Close"));
  auto take = reinterpret_cast<CameraName>(GetProcAddress(dll, "SimConnect_CameraAcquire"));
  auto release = reinterpret_cast<CameraName>(GetProcAddress(dll, "SimConnect_CameraRelease"));
  auto get = reinterpret_cast<CameraGet>(GetProcAddress(dll, "SimConnect_CameraGet"));
  auto dispatch = reinterpret_cast<Dispatch>(GetProcAddress(dll, "SimConnect_GetNextDispatch"));
  if (!open || !close || !take || !release || !get || !dispatch) { FreeLibrary(dll); return 2; }
  HANDLE session = nullptr;
  HRESULT hr = open(&session, "Taxi Cam bounded public camera read", nullptr, 0, nullptr, 0);
  if (FAILED(hr)) { std::printf("{\"open_error\":%ld}\n", hr); FreeLibrary(dll); return 1; }
  if (acquire) { hr = take(session, "Taxi Cam bounded camera calibration"); std::printf("{\"acquire_hr\":%ld}\n", hr); }
  unsigned packets = 0;
  for (DWORD reference = 1; SUCCEEDED(hr) && reference <= 3; ++reference) {
    if (acquire && reference == 1) Sleep(300);
    hr = get(session, reference);
    std::printf("{\"requested_reference\":%lu,\"hresult\":%ld}\n", reference, hr);
    const auto start = GetTickCount64();
    while (GetTickCount64() - start < 1000 && packets < 32) {
      void* packet = nullptr; DWORD bytes = 0;
      if (SUCCEEDED(dispatch(session, &packet, &bytes)) && packet && bytes >= 12 && bytes <= 65536) {
        std::array<DWORD, 3> header{}; std::memcpy(header.data(), packet, 12);
        if (header[0] < 12 || header[0] > bytes) break;
        ++packets;
        // Open payload contains strings and is intentionally omitted.
        std::printf("{\"reference_request\":%lu,\"id\":%lu,\"bytes\":%lu,\"payload_u32\":[", reference, header[2], header[0]);
        const auto payload = static_cast<const unsigned char*>(packet) + 12;
        const auto limit = header[2] == 2 ? 0u : (header[0] - 12 > 128 ? 128u : header[0] - 12);
        for (unsigned off = 0; off + 4 <= limit; off += 4) { DWORD value = 0; std::memcpy(&value, payload + off, 4); std::printf("%s%lu", off ? "," : "", value); }
        std::printf("]}\n"); std::fflush(stdout);
      }
      Sleep(10);
    }
  }
  if (acquire) { auto released = release(session, ""); std::printf("{\"release_hr\":%ld}\n", released); Sleep(250); }
  auto closed = close(session); std::printf("{\"close_hr\":%ld,\"packets\":%u}\n", closed, packets);
  FreeLibrary(dll);
  return SUCCEEDED(hr) && packets ? 0 : 1;
}
