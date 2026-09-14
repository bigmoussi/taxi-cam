// Disposable debugger capability check. No simulator access.
#include <windows.h>
extern "C" __declspec(noinline) unsigned taxi_debug_fixture(unsigned value) { return value + 1; }
DWORD WINAPI fixture_worker(void*) {
  unsigned count = 0;
  for (unsigned index = 0; index != 6000; ++index) {
    count = taxi_debug_fixture(count);
    Sleep(1);
  }
  return count == 6000 ? 0 : 1;
}
int main(int argc, char**) {
  if (argc > 1) {
    for (unsigned index = 0; index != 16; ++index) {
      HANDLE thread = CreateThread(nullptr, 0, fixture_worker, nullptr, 0, nullptr);
      if (!thread) return 2;
      CloseHandle(thread);
    }
  }
  unsigned count = 0;
  for (unsigned index = 0; index != 6000; ++index) {
    count = taxi_debug_fixture(count);
    Sleep(10);
  }
  return count == 6000 ? 0 : 1;
}
