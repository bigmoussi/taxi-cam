#include "../../src/camera/local_memory.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
using namespace taxi_camera::native_camera;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
struct Faults {
  explicit Faults(LocalMemoryQueryTestFaults value) { set_local_memory_query_test_faults(value); }
  ~Faults() { set_local_memory_query_test_faults({}); }
};
struct Pages {
  std::size_t page;
  std::uint8_t* data;
  Pages() {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    page = info.dwPageSize;
    data = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, page * 3, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    require(data != nullptr, "allocation failed");
    std::memset(data, 0x59, page * 3);
    require(VirtualLock(data, page * 3) != FALSE, "could not pin deterministic page fixture");
  }
  ~Pages() {
    VirtualUnlock(data, page * 3);
    VirtualFree(data, 0, MEM_RELEASE);
  }
  std::uint64_t at(std::size_t offset = 0) const { return reinterpret_cast<std::uintptr_t>(data + offset); }
  void protect(std::size_t offset, DWORD value) {
    DWORD before = 0;
    require(VirtualProtect(data + offset, page, value, &before) != FALSE, "could not change fixture protection");
  }
};
void invalid_arguments() {
  Pages fixture;
  LocalMemoryMetrics metrics;
  ScopedLocalMemoryMetrics measure(metrics);
  for (const auto address : {std::uint64_t(0), UINT64_MAX, UINT64_MAX - 7})
    require(!writable_private_span(address, 16), "invalid address accepted");
  require(!writable_private_span(fixture.at(), 0) && !writable_private_span(fixture.at(), 17), "unbounded span accepted");
  require(metrics.query_calls == 0 && metrics.read_calls == 0, "invalid input reached OS queries/reads");
  require(writable_private_span(fixture.at(), 1), "one-byte bounded span refused");
}
void fresh_and_fallback() {
  for (const auto fault : {LocalMemoryQueryTestFaults{}, LocalMemoryQueryTestFaults{true, false, false},
                           LocalMemoryQueryTestFaults{false, true, false}, LocalMemoryQueryTestFaults{false, false, true}}) {
    Pages fixture;
    Faults inject(fault);
    for (const auto offset : {std::size_t(0), fixture.page - 8}) {
      LocalMemoryMetrics metrics;
      ScopedLocalMemoryMetrics measure(metrics);
      require(writable_private_span(fixture.at(offset), 16), "RW span refused");
      require(metrics.read_calls == 0 && metrics.requested_bytes == 0, "metadata validation read flag contents");
      require(metrics.query_calls == metrics.query_allocation_calls + metrics.query_page_calls + metrics.query_fallback_calls,
              "query family accounting mismatch");
      const bool fallback = fault.allocation_unavailable || fault.pages_unavailable || fault.pages_nonresident;
      require((metrics.query_fallback_calls != 0) == fallback, "unexpected fallback selection");
      if (!fallback)
        require(metrics.query_allocation_calls == 1 && metrics.query_page_calls == 1 && metrics.query_calls == 2,
                "hot span did not batch both page queries");
      fixture.protect(fixture.page, PAGE_READONLY);
      require(!writable_private_span(fixture.at(fixture.page - 8), 16), "second readonly flag word accepted");
      fixture.protect(fixture.page, PAGE_READWRITE);
    }
    fixture.protect(0, PAGE_READONLY);
    require(!writable_private_span(fixture.at(), 16), "fresh query reused old writable metadata");
    fixture.protect(0, PAGE_READWRITE);
    LocalMemoryReader reader(16);
    std::uint64_t word = 0;
    ScopedLocalMemoryQueryCache old(LocalMemoryQueryMode::private_pages);
    require(reader.read(fixture.at(), &word, sizeof(word)), "read-only proof setup failed");
    fixture.protect(0, PAGE_READONLY);
    require(!writable_private_span(fixture.at(), 16), "writable guard borrowed an active stale inspection proof");
    require(!old.finish(), "changed inspection metadata accepted");
    fixture.protect(0, PAGE_READWRITE);
  }
}
void separate_allocations() {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  const auto granularity = std::size_t(info.dwAllocationGranularity);
  auto* address = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, granularity * 2, MEM_RESERVE, PAGE_NOACCESS));
  require(address != nullptr && VirtualFree(address, 0, MEM_RELEASE), "adjacency reservation failed");
  auto* first = VirtualAlloc(address, granularity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  auto* second = VirtualAlloc(address + granularity, granularity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  require(first == address && second == address + granularity, "adjacent allocation fixture failed");
  std::memset(address + granularity - 8, 0x45, 16);
  for (const auto fault : {LocalMemoryQueryTestFaults{}, LocalMemoryQueryTestFaults{true, false, false},
                           LocalMemoryQueryTestFaults{false, true, false}, LocalMemoryQueryTestFaults{false, false, true}}) {
    Faults inject(fault);
    require(!writable_private_span(reinterpret_cast<std::uintptr_t>(address + granularity - 8), 16),
            "two distinct RW allocations passed single-allocation guard");
  }
  require(VirtualFree(first, 0, MEM_RELEASE) && VirtualFree(second, 0, MEM_RELEASE), "adjacency cleanup failed");
}
void exact_reads() {
  Pages fixture;
  std::array<std::uint64_t, 2> output{1, 2};
  LocalMemoryMetrics metrics;
  ScopedLocalMemoryMetrics measure(metrics);
  require(read_local_flag_words(fixture.at(fixture.page - 8), output), "exact cross-page flags read failed");
  require(output[0] == 0x5959595959595959ull && output[1] == output[0] && metrics.read_calls == 1 && metrics.requested_bytes == 16,
          "exact flags read/accounting changed");
  fixture.protect(fixture.page, PAGE_NOACCESS);
  output = {1, 2};
  require(!read_local_flag_words(fixture.at(fixture.page - 8), output) && output == std::array<std::uint64_t, 2>{1, 2},
          "failed cross-page read exposed partial output");
  require(metrics.read_calls == 2 && metrics.requested_bytes == 32, "failed read did not consume exact metrics");
  fixture.protect(fixture.page, PAGE_READWRITE);
  require(!read_local_flag_words(0, output) && !read_local_flag_words(UINT64_MAX, output) && metrics.read_calls == 2,
          "invalid flags request reached RPM");
}
}  // namespace
int main() {
  try {
    invalid_arguments();
    fresh_and_fallback();
    separate_allocations();
    exact_reads();
    std::printf("Writable private span tests passed: %u checks\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s (Windows error %lu)\n", error.what(), GetLastError());
    return 1;
  }
}
