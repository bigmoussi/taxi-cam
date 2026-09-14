// Fixed aircraft pose and eyepoint read-only public values, one bounded request.
// Signatures and enum order: official MSFS SDK SimConnect_Open,
// SimConnect_AddToDataDefinition, SimConnect_RequestDataOnSimObject,
// SimConnect_GetNextDispatch, SIMCONNECT_DATATYPE and SIMCONNECT_RECV_ID.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <cmath>
using Open = HRESULT(WINAPI*)(HANDLE*, const char*, HWND, DWORD, HANDLE, DWORD);
using Close = HRESULT(WINAPI*)(HANDLE);
using Define = HRESULT(WINAPI*)(HANDLE, DWORD, const char*, const char*, DWORD, float, DWORD);
using Request = HRESULT(WINAPI*)(HANDLE, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
using Dispatch = HRESULT(WINAPI*)(HANDLE, void**, DWORD*);
int main() {
  const auto dll = LoadLibraryExW(L"C:/Program Files/Little Navmap/SimConnect_msfs_2024.dll", nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!dll) { std::printf("{\"complete\":false,\"error\":\"load\",\"code\":%lu}\n", GetLastError()); return 1; }
  const auto open = reinterpret_cast<Open>(GetProcAddress(dll, "SimConnect_Open"));
  const auto close = reinterpret_cast<Close>(GetProcAddress(dll, "SimConnect_Close"));
  const auto define = reinterpret_cast<Define>(GetProcAddress(dll, "SimConnect_AddToDataDefinition"));
  const auto request = reinterpret_cast<Request>(GetProcAddress(dll, "SimConnect_RequestDataOnSimObject"));
  const auto dispatch = reinterpret_cast<Dispatch>(GetProcAddress(dll, "SimConnect_GetNextDispatch"));
  if (!open || !close || !define || !request || !dispatch) { FreeLibrary(dll); return 2; }
  HANDLE session = nullptr;
  HRESULT hr = open(&session, "380 Taxi Cam read-only aircraft pose comparison", nullptr, 0, nullptr, 0);
  const char* names[]{"PLANE LATITUDE", "PLANE LONGITUDE", "PLANE ALTITUDE", "PLANE PITCH DEGREES", "PLANE BANK DEGREES", "PLANE HEADING DEGREES TRUE"};
  for (unsigned i = 0; SUCCEEDED(hr) && i < 6; ++i)
    hr = define(session, 1, names[i], i == 2 ? "meters" : "degrees", 4, 0, 0xffffffffu);
  const char* structs[]{"EYEPOINT POSITION", "STRUCT EYEPOINT DYNAMIC OFFSET", "STRUCT EYEPOINT DYNAMIC ANGLE"};
  for (const auto name : structs)
    if (SUCCEEDED(hr)) hr = define(session, 1, name, nullptr, 16, 0, 0xffffffffu);
  if (SUCCEEDED(hr)) hr = request(session, 1, 1, 0, 1, 0, 0, 0, 0);
  bool complete = false;
  DWORD exception = 0;
  std::array<double, 15> values{};
  const auto start = GetTickCount64();
  while (SUCCEEDED(hr) && !complete && GetTickCount64() - start < 5000) {
    void* packet = nullptr;
    DWORD bytes = 0;
    if (SUCCEEDED(dispatch(session, &packet, &bytes)) && packet && bytes >= 12 && bytes <= 65536) {
      std::array<DWORD, 10> header{};
      std::memcpy(header.data(), packet, bytes >= 40 ? 40 : 12);
      if (header[0] > bytes || header[0] < 12) break;
      if (header[2] == 1 && bytes >= 16) { std::memcpy(&exception, static_cast<unsigned char*>(packet) + 12, 4); break; }
      if (header[2] == 8 && header[3] == 1 && header[5] == 1 && header[9] == 9 && header[0] >= 160 && bytes >= 160) {
        std::memcpy(values.data(), static_cast<unsigned char*>(packet) + 40, 120);
        complete = true;
        for (const auto value : values) complete &= std::isfinite(value);
      }
    }
    if (!complete) Sleep(10);
  }
  if (session) close(session);
  FreeLibrary(dll);
  if (!complete) { std::printf("{\"complete\":false,\"hresult\":%ld,\"exception\":%lu}\n", hr, exception); return 1; }
  std::printf("{\"complete\":true,\"latitude_degrees\":%.17g,\"longitude_degrees\":%.17g,\"altitude_meters\":%.17g,\"pitch_degrees\":%.17g,\"bank_degrees\":%.17g,\"true_heading_degrees\":%.17g,\"eyepoint_feet\":[%.17g,%.17g,%.17g],\"eyepoint_dynamic_feet\":[%.17g,%.17g,%.17g],\"eyepoint_dynamic_radians\":[%.17g,%.17g,%.17g]}\n",
    values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8], values[9], values[10], values[11], values[12], values[13], values[14]);
  return 0;
}
