#include "../../src/camera/local_memory.hpp"

#include <psapi.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>

namespace {
using namespace taxi_camera::native_camera;
#ifndef TAXI_LOCAL_MEMORY_TESTING
#error Image-page tests require the scoped memory-query fault seam.
#endif
constexpr std::size_t Page = 4096;
// Whole, aligned data pages in this test's PE image only. No code, import table,
// runtime metadata or another process is protected or written by these tests.
alignas(Page) std::array<std::uint8_t, Page*(ScopedLocalMemoryQueryCache::kPageLimit + 8)> image_data{};
alignas(Page) std::array<std::uint8_t, Page * 16> cold_image_data{};
unsigned checks = 0;
bool cold_fallback_exercised = false;

void require(bool value, const char* message) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL: %s (Windows error %lu)\n", message, GetLastError());
    std::abort();
  }
}
void accounting(const LocalMemoryMetrics& metrics) {
  require(metrics.query_calls == metrics.query_allocation_calls + metrics.query_page_calls + metrics.query_fallback_calls,
          "Image metadata query families do not sum to real OS calls");
}
bool filled(const auto& bytes, std::uint8_t value) {
  return std::all_of(bytes.begin(), bytes.end(), [=](auto byte) { return byte == value; });
}
PSAPI_WORKING_SET_EX_INFORMATION page_info(void* address) {
  PSAPI_WORKING_SET_EX_INFORMATION info{};
  info.VirtualAddress = address;
  require(K32QueryWorkingSetEx(GetCurrentProcess(), &info, sizeof(info)) != FALSE, "Could not inspect own image residency");
  return info;
}
struct Protection {
  void* address;
  DWORD previous = 0;
  Protection(void* target, DWORD protection) : address(target) {
    require(VirtualProtect(address, Page, protection, &previous) != FALSE, "Could not protect an isolated test image-data page");
  }
  ~Protection() {
    DWORD ignored = 0;
    require(VirtualProtect(address, Page, previous, &ignored) != FALSE, "Could not restore test image-data protection");
  }
};
struct LockedPage {
  void* address;
  explicit LockedPage(void* target) : address(target) {
    require(VirtualLock(address, Page) != FALSE, "Could not pin a small deterministic image-data fixture");
  }
  ~LockedPage() { require(VirtualUnlock(address, Page) != FALSE, "Could not release the test image-data residency pin"); }
};
struct QueryFault {
  explicit QueryFault(LocalMemoryQueryTestFaults faults) { set_local_memory_query_test_faults(faults); }
  ~QueryFault() { set_local_memory_query_test_faults({}); }
};
struct PrivateAllocation {
  std::uint8_t* data = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, Page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  PrivateAllocation() {
    require(data != nullptr, "Could not create private impostor fixture");
    std::fill_n(data, Page, 0x57);
  }
  ~PrivateAllocation() { require(VirtualFree(data, 0, MEM_RELEASE) != FALSE, "Could not release private impostor fixture"); }
};
struct Image {
  HMODULE module = GetModuleHandleW(nullptr);
  std::uintptr_t base = reinterpret_cast<std::uintptr_t>(module);
  std::uint32_t size = 0;
  std::uint32_t data_rva = 0;
  Image() {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    require(system.dwPageSize == Page && reinterpret_cast<std::uintptr_t>(image_data.data()) % Page == 0 &&
                reinterpret_cast<std::uintptr_t>(cold_image_data.data()) % Page == 0,
            "Dedicated PE fixture is not aligned to the actual host page size");
    const auto image = parse_verified_main_image();
    require(image.valid_image && image.image_size != 0, "Could not establish bounded main-image metadata");
    size = image.image_size;
    const auto address = reinterpret_cast<std::uintptr_t>(image_data.data());
    require(address >= base && address - base < size && image_data.size() <= size - (address - base),
            "Test data is outside the main executable image");
    data_rva = static_cast<std::uint32_t>(address - base);
    MEMORY_BASIC_INFORMATION region{};
    require(VirtualQuery(image_data.data(), &region, sizeof(region)) == sizeof(region) && region.Type == MEM_IMAGE &&
                region.AllocationBase == module && region.State == MEM_COMMIT && !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS)),
            "Dedicated test data lacks committed main-image identity");
  }
  LocalImageReader pages() const { return {module, size, LocalImageQueryMode::pages}; }
  std::uint32_t rva(std::size_t offset = 0) const { return data_rva + static_cast<std::uint32_t>(offset); }
};

void fresh_reads_and_bounds(const Image& image) {
  std::fill_n(image_data.data(), Page * 4, 0x39);
  LockedPage locked(image_data.data());
  auto reader = image.pages();
  LocalMemoryMetrics metrics;
  ScopedLocalMemoryMetrics measured(metrics);
  std::array<std::uint8_t, 16> output{};
  const auto window = reader.query(image.rva(17), static_cast<std::uint32_t>(Page * 2));
  require(window.readable && window.size == Page - 17, "Explicit image page query did not clip at its page boundary");
  const auto queries = metrics.query_calls;
  require(reader.read(image.rva(17), output.data(), output.size()) && filled(output, 0x39) && metrics.query_calls > queries,
          "A direct image read reused metadata from an earlier independent query");
  const auto reads = metrics.read_calls;
  const auto queried = metrics.query_calls;
  image_data[17] = 0x6a;
  require(reader.read(image.rva(17), output.data(), output.size()) && output[0] == 0x6a && metrics.query_calls > queried &&
              metrics.read_calls == reads + 1,
          "Fresh direct image reread skipped current metadata or cached contents");
  image_data[17] = 0x39;
  require(metrics.query_allocation_calls != 0 && metrics.query_page_calls != 0 && metrics.query_fallback_calls == 0,
          "Hot explicit image reads did not use allocation/page metadata");
  const auto before = metrics.query_calls;
  const auto read_before = metrics.read_calls;
  output.fill(0xad);
  require(reader.query(image.size, 1).size == 0 && reader.query(0, 0).size == 0 && !reader.read(image.size - 1, output.data(), 2) &&
              !reader.read(0, nullptr, 1) && !reader.read(0, output.data(), 0) &&
              !reader.read(0, output.data(), std::numeric_limits<std::size_t>::max()) && metrics.query_calls == before &&
              metrics.read_calls == read_before && filled(output, 0xad),
          "Invalid image bounds issued OS calls or changed output");
  LocalImageReader tiny(image.module, image.rva(21), LocalImageQueryMode::pages);
  require(
      tiny.query(image.rva(17), 64).size == 4 && tiny.read(image.rva(17), output.data(), 4) && !tiny.read(image.rva(17), output.data(), 5),
      "Image page extent escaped the caller-supplied upper bound");
  LocalImageReader oversized(image.module, 0x80000000, LocalImageQueryMode::pages);
  require(!oversized.query(image.size, 8).readable && !oversized.read(image.size, output.data(), 8),
          "Oversized image bound admitted memory beyond the real main allocation");
  accounting(metrics);
}

void module_identity_and_private_isolation(const Image& image) {
  PrivateAllocation allocation;
  HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, Page, nullptr);
  require(mapping != nullptr, "Could not create anonymous mapped impostor fixture");
  void* mapped = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, Page);
  require(mapped != nullptr, "Could not map the impostor fixture");
  LocalMemoryMetrics metrics;
  ScopedLocalMemoryMetrics measured(metrics);
  for (const auto module : {static_cast<HMODULE>(nullptr), GetModuleHandleW(L"kernel32.dll"), reinterpret_cast<HMODULE>(allocation.data),
                            reinterpret_cast<HMODULE>(mapped)}) {
    LocalImageReader impostor(module, Page, LocalImageQueryMode::pages);
    std::array<std::uint8_t, 8> output{};
    output.fill(0xad);
    require(impostor.query(0, 8).size == 0 && !impostor.read(0, output.data(), output.size()) && filled(output, 0xad),
            "Foreign, private or mapped memory became a main-image reader");
  }
  LocalImageReader zero(image.module, 0, LocalImageQueryMode::pages);
  LocalImageReader excessive(image.module, 0xffffffff, LocalImageQueryMode::pages);
  require(zero.query(0, 1).size == 0 && excessive.query(0, 1).size == 0 && metrics.query_calls == 0 && metrics.read_calls == 0,
          "Rejected module identities or constructor bounds performed OS memory access");
  require(UnmapViewOfFile(mapped) != FALSE && CloseHandle(mapping) != FALSE, "Could not release mapped impostor fixture");
  {
    ScopedLocalMemoryQueryCache scope(LocalMemoryQueryMode::private_pages);
    auto reader = image.pages();
    LocalMemoryReader objects;
    std::uint64_t value = 0;
    require(reader.read(image.rva(), &value, sizeof(value)) && value == 0x3939393939393939ull &&
                objects.read(reinterpret_cast<std::uintptr_t>(allocation.data), &value, sizeof(value)) && value == 0x5757575757575757ull,
            "A mixed image/private transaction lost a valid allocation type");
    require(scope.finish(), "Mixed typed allocation/page proofs failed unchanged endpoint validation");
  }
  {
    ScopedLocalMemoryQueryCache scope(LocalMemoryQueryMode::private_pages);
    auto reader = image.pages();
    LocalMemoryReader objects;
    std::uint64_t value = 0;
    require(reader.read(image.rva(), &value, sizeof(value)), "Image proof setup failed");
    value = 0xadadadadadadadadull;
    require(!objects.read(reinterpret_cast<std::uintptr_t>(image_data.data()), &value, sizeof(value)) && value == 0xadadadadadadadadull,
            "Private reader consumed image-typed cached page evidence");
  }
  {
    ScopedLocalMemoryQueryCache scope(LocalMemoryQueryMode::private_pages);
    LocalMemoryReader objects;
    std::uint64_t value = 0;
    const auto address = reinterpret_cast<std::uintptr_t>(allocation.data);
    require(objects.read(address, &value, sizeof(value)), "Private proof setup for reverse type isolation failed");
    const auto queries = metrics.query_calls;
    require(!scope.validate_image_range(address, sizeof(value), image.base) && metrics.query_calls == queries && !scope.finish(),
            "Image validator consumed cached private-page identity or retried a known type mismatch");
  }
  accounting(metrics);
}

void protections_and_output(const Image& image) {
  auto reader = image.pages();
  auto* const target = image_data.data() + Page;
  std::fill_n(target, Page, 0x39);
  for (const auto protection : {DWORD(PAGE_READONLY), DWORD(PAGE_READWRITE), DWORD(PAGE_WRITECOPY), DWORD(PAGE_EXECUTE_READ)}) {
    Protection protected_page(target, protection);
    std::array<std::uint8_t, 8> output{};
    require(reader.query(image.rva(Page), 8).readable && reader.read(image.rva(Page), output.data(), output.size()) && filled(output, 0x39),
            "Readable main-image page protection was rejected");
  }
  for (const auto protection : {DWORD(PAGE_NOACCESS), DWORD(PAGE_READWRITE | PAGE_GUARD), DWORD(PAGE_EXECUTE)}) {
    Protection protected_page(target, protection);
    LocalMemoryMetrics metrics;
    ScopedLocalMemoryMetrics measured(metrics);
    std::array<std::uint8_t, 16> output;
    output.fill(0xad);
    const auto window = reader.query(image.rva(Page + 7), 64);
    require(!window.readable && window.size == 64 && !reader.read(image.rva(Page + 7), output.data(), output.size()) &&
                filled(output, 0xad) && metrics.read_calls == 0,
            "Protected/execute-only image page was read or failed its bounded unreadable-window contract");
    MEMORY_BASIC_INFORMATION actual{};
    require(VirtualQuery(target, &actual, sizeof(actual)) == sizeof(actual) && actual.Protect == protection,
            "Image metadata query consumed a guard or changed protection");
    output.fill(0xad);
    require(!reader.read(image.rva(Page - 8), output.data(), output.size()) &&
                std::all_of(output.begin(), output.begin() + 8, [](auto b) { return b == 0x39; }) &&
                std::all_of(output.begin() + 8, output.end(), [](auto b) { return b == 0xad; }),
            "Cross-page failure changed the existing image reader's partial-output behavior");
    accounting(metrics);
  }
  std::array<std::uint8_t, 16> across{};
  require(reader.read(image.rva(Page - 8), across.data(), across.size()) && filled(across, 0x39),
          "Readable image field spanning pages was not read exactly");
  // A modified image-data page is still MEM_IMAGE, even when its physical page
  // is no longer shared. Shared is not an image-identity requirement.
  target[0] = 0x52;
  const auto copy = page_info(target);
  require(copy.VirtualAttributes.Valid && !copy.VirtualAttributes.Shared, "Own image write did not create a resident private copy");
  std::uint8_t byte = 0;
  require(reader.read(image.rva(Page), &byte, 1) && byte == 0x52, "Unshared image COW data lost image identity");
  target[0] = 0x39;
}

void transaction_endpoints(const Image& image) {
  auto reader = image.pages();
  LockedPage locked(image_data.data());
  LocalMemoryMetrics metrics;
  ScopedLocalMemoryMetrics measured(metrics);
  std::uint64_t first = 0, second = 0;
  {
    ScopedLocalMemoryQueryCache scope(LocalMemoryQueryMode::private_pages);
    require(reader.read(image.rva(), &first, sizeof(first)), "Image transaction setup failed");
    const auto queries = metrics.query_calls;
    image_data[0] ^= 1;
    require(reader.read(image.rva(), &second, sizeof(second)) && first != second && metrics.query_calls == queries,
            "Image transaction cached bytes or repeated hot metadata queries");
    image_data[0] ^= 1;
    require(scope.finish() && metrics.query_calls > queries && !scope.finish(),
            "Image transaction did not revalidate once and detach at its endpoint");
  }
  const auto queries = metrics.query_calls;
  require(reader.read(image.rva(), &second, sizeof(second)) && metrics.query_calls > queries,
          "Finished transaction leaked cached image proofs");
  {
    ScopedLocalMemoryQueryCache scope(LocalMemoryQueryMode::private_pages);
    require(reader.read(image.rva(), &first, sizeof(first)), "Image endpoint-change setup failed");
    Protection changed(image_data.data(), PAGE_READONLY);
    require(!scope.finish() && metrics.query_cache_validation_failures != 0,
            "A requested image page changed protection without failing its endpoint");
  }
  {
    ScopedLocalMemoryQueryCache scope(LocalMemoryQueryMode::private_pages);
    require(reader.read(image.rva(), &first, sizeof(first)), "Unread-page-change setup failed");
    Protection unrelated(image_data.data() + Page * 3, PAGE_READONLY);
    require(scope.finish(), "Unrequested image-page change was mistaken for requested-page proof failure");
  }
  // The narrower guarantee is explicit: a direct MBI observation still records
  // the entire homogeneous suffix and must notice an unrelated-page split.
  {
    ScopedLocalMemoryQueryCache scope(LocalMemoryQueryMode::private_pages);
    MEMORY_BASIC_INFORMATION region{};
    require(scope.query(image_data.data(), region) == sizeof(region), "Direct image MBI setup failed");
    const auto begin = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    const auto changed = reinterpret_cast<std::uintptr_t>(image_data.data() + Page * 3);
    require(begin <= changed && changed - begin < region.RegionSize, "Full-region fixture lacks the expected homogeneous suffix");
    Protection unrelated(image_data.data() + Page * 3, PAGE_READONLY);
    require(!scope.finish(), "Direct MBI query silently acquired the narrower image-page endpoint semantics");
  }
  accounting(metrics);
}

void scope_modes_and_thread_isolation(const Image& image) {
  LockedPage locked(image_data.data());
  auto reader = image.pages();
  LocalMemoryMetrics metrics;
  ScopedLocalMemoryMetrics measured(metrics);
  std::uint64_t value = 0;
  {
    LocalImageReader legacy(image.module, image.size);
    ScopedLocalMemoryQueryCache pages(LocalMemoryQueryMode::private_pages);
    require(legacy.read(image.rva(), &value, sizeof(value)) && pages.finish() && metrics.query_fallback_calls != 0 &&
                metrics.query_page_calls == 0 && metrics.query_allocation_calls == 0,
            "Default image reader changed the legacy scanner/query contract");
  }
  metrics = {};
  ScopedLocalMemoryQueryCache outer(LocalMemoryQueryMode::private_pages);
  require(reader.read(image.rva(), &value, sizeof(value)) && outer.is_current(), "Outer image-page scope setup failed");
  {
    ScopedLocalMemoryQueryCache inner;
    const auto fallback = metrics.query_fallback_calls;
    const auto pages = metrics.query_page_calls;
    require(!outer.is_current() && reader.read(image.rva(), &value, sizeof(value)) && metrics.query_fallback_calls > fallback &&
                metrics.query_page_calls == pages && inner.finish(),
            "Explicit image mode bypassed an active full-region transaction");
  }
  require(outer.is_current(), "Nested legacy scope did not restore image-page transaction");
  LocalMemoryMetrics other_metrics;
  bool other_passed = false;
  std::thread other([&] {
    ScopedLocalMemoryMetrics measured_other(other_metrics);
    ScopedLocalMemoryQueryCache independent(LocalMemoryQueryMode::private_pages);
    auto independent_reader = image.pages();
    std::uint64_t output = 0;
    other_passed = independent_reader.read(image.rva(), &output, sizeof(output)) && output == value && independent.finish();
  });
  other.join();
  const auto queries = metrics.query_calls;
  require(other_passed && other_metrics.query_page_calls != 0 && other_metrics.query_allocation_calls != 0 &&
              reader.read(image.rva(), &value, sizeof(value)) && metrics.query_calls == queries && outer.finish(),
          "Nested/thread-local image metadata escaped its own transaction");
  accounting(other_metrics);
  accounting(metrics);
}

void capacity_and_cold_fallback(const Image& image) {
  auto reader = image.pages();
  for (std::size_t p = 0; p <= ScopedLocalMemoryQueryCache::kPageLimit; ++p)
    image_data[p * Page] = 0x39;
  for (bool change_first : {false, true}) {
    LocalMemoryMetrics metrics;
    ScopedLocalMemoryMetrics measured(metrics);
    ScopedLocalMemoryQueryCache scope(LocalMemoryQueryMode::private_pages);
    std::uint8_t value = 0;
    for (std::size_t p = 0; p <= ScopedLocalMemoryQueryCache::kPageLimit; ++p)
      require(reader.read(image.rva(p * Page), &value, 1) && value == 0x39, "Image page-cap fallback lost exact field bytes");
    require(metrics.query_fallback_calls != 0, "Image proof capacity silently expanded instead of retaining bounded fallback");
    if (change_first) {
      Protection changed(image_data.data(), PAGE_READONLY);
      require(!scope.finish(), "Image capacity fallback discarded a prior requested-page proof");
    } else {
      require(scope.finish(), "Image capacity fallback refused unchanged metadata");
    }
    accounting(metrics);
  }
  // Residency is OS-controlled. Exercise an untouched own-image page only when
  // it is actually nonresident; never trim the process/system working set.
  auto* cold = cold_image_data.data() + Page * 8;
  const auto cold_rva = reinterpret_cast<std::uintptr_t>(cold) - image.base;
  require(cold_rva < image.size && Page <= image.size - cold_rva, "Cold fixture is outside the main image");
  // Isolate the legacy suffix from adjacent writable image data that the CRT
  // can copy-on-write while the test runs. Such a split correctly fails the
  // full-region endpoint and is not the residency fallback being tested here.
  Protection cold_boundary(cold, PAGE_READONLY);
  if (!page_info(cold).VirtualAttributes.Valid) {
    LocalMemoryMetrics metrics;
    ScopedLocalMemoryMetrics measured(metrics);
    ScopedLocalMemoryQueryCache scope(LocalMemoryQueryMode::private_pages);
    const auto window = reader.query(static_cast<std::uint32_t>(cold_rva), 16);
    std::array<std::uint8_t, 16> output{};
    const bool read = reader.read(static_cast<std::uint32_t>(cold_rva), output.data(), output.size());
    const bool finished = scope.finish();
    if (!window.readable || window.size != output.size() || !read || !filled(output, 0) || !finished)
      std::fprintf(stderr, "Cold image diagnostic: readable=%d size=%u read=%d zero=%d finish=%d fallback=%llu; %s\n", window.readable,
                   window.size, read, filled(output, 0), finished, static_cast<unsigned long long>(metrics.query_fallback_calls),
                   describe_local_memory_query_failure(scope.failure()).c_str());
    require(window.readable && window.size == output.size() && read && filled(output, 0) && finished,
            "Cold image page did not retain the legacy committed-image fallback");
    // The OS can make the page resident between the independent observation and
    // query; deterministic nonresident/API tests below cover that exact branch.
    cold_fallback_exercised = metrics.query_fallback_calls != 0;
    accounting(metrics);
  }
}

void deterministic_api_fallback(const Image& image) {
  auto reader = image.pages();
  const std::array<LocalMemoryQueryTestFaults, 3> faults{{{true, false, false}, {false, true, false}, {false, false, true}}};
  for (const auto fault : faults) {
    QueryFault injected(fault);
    LocalMemoryMetrics metrics;
    ScopedLocalMemoryMetrics measured(metrics);
    std::uint64_t value = 0;
    require(reader.read(image.rva(), &value, sizeof(value)) && value == 0x3939393939393939ull && metrics.query_fallback_calls != 0,
            "Unavailable/invalid direct page metadata did not fall back to exact image MBI checks");
    {
      ScopedLocalMemoryQueryCache scope(LocalMemoryQueryMode::private_pages);
      require(reader.read(image.rva(), &value, sizeof(value)) && scope.finish(),
              "Initial image API/residency fallback failed unchanged region endpoint checks");
    }
    {
      ScopedLocalMemoryQueryCache scope(LocalMemoryQueryMode::private_pages);
      require(reader.read(image.rva(), &value, sizeof(value)), "Fallback protection-change setup failed");
      Protection changed(image_data.data(), PAGE_READONLY);
      require(!scope.finish(), "Initial fallback dropped the saved region protection proof");
    }
    {
      PrivateAllocation wrong_type;
      ScopedLocalMemoryQueryCache scope(LocalMemoryQueryMode::private_pages);
      require(!scope.validate_image_range(reinterpret_cast<std::uintptr_t>(wrong_type.data), 8, image.base) && !scope.finish(),
              "Unavailable image page API made a private allocation count as the main image");
    }
    {
      Protection guarded(image_data.data() + Page, PAGE_READWRITE | PAGE_GUARD);
      const auto before = metrics.read_calls;
      std::array<std::uint8_t, 8> output;
      output.fill(0xad);
      require(!reader.read(image.rva(Page), output.data(), output.size()) && filled(output, 0xad) && metrics.read_calls == before,
              "An API fallback consumed guarded image data");
      MEMORY_BASIC_INFORMATION actual{};
      require(VirtualQuery(image_data.data() + Page, &actual, sizeof(actual)) == sizeof(actual) && (actual.Protect & PAGE_GUARD),
              "API fallback cleared the image fixture guard");
    }
    accounting(metrics);
  }
  LockedPage locked(image_data.data());
  for (const auto fault : {LocalMemoryQueryTestFaults{false, true, false}, LocalMemoryQueryTestFaults{false, false, true}}) {
    for (bool change_requested : {false, true}) {
      LocalMemoryMetrics metrics;
      ScopedLocalMemoryMetrics measured(metrics);
      ScopedLocalMemoryQueryCache scope(LocalMemoryQueryMode::private_pages);
      std::uint64_t value = 0;
      require(reader.read(image.rva(), &value, sizeof(value)) && metrics.query_page_calls != 0 && metrics.query_fallback_calls == 0,
              "Endpoint-only fallback did not start from real hot page metadata");
      QueryFault injected(fault);
      if (change_requested) {
        Protection changed(image_data.data(), PAGE_READONLY);
        require(!scope.finish(), "Endpoint page API fallback accepted changed image protection");
      } else {
        require(scope.finish(), "Endpoint residency/API fallback rejected unchanged committed image protection");
      }
      require(metrics.query_fallback_calls != 0, "Endpoint page metadata failure did not obtain a fresh MBI proof");
      accounting(metrics);
    }
  }
  {
    ScopedLocalMemoryQueryCache scope(LocalMemoryQueryMode::private_pages);
    std::uint64_t value = 0;
    require(reader.read(image.rva(), &value, sizeof(value)), "Allocation-endpoint API failure setup failed");
    QueryFault injected({true, false, false});
    require(!scope.finish(), "Unavailable allocation endpoint reused the initial image identity/extent as fresh proof");
  }
}
}  // namespace

int main() {
  const Image image;
  fresh_reads_and_bounds(image);
  module_identity_and_private_isolation(image);
  protections_and_output(image);
  transaction_endpoints(image);
  scope_modes_and_thread_isolation(image);
  capacity_and_cold_fallback(image);
  deterministic_api_fallback(image);
  std::printf(
      "PASS: %u image-page memory checks; real main-image identity, COW/protections, exact reads, typed scoped endpoints, "
      "legacy/nested/thread isolation and bounded fallback. Cold image fallback exercised: %s.\n",
      checks, cold_fallback_exercised ? "yes" : "no (fixture was resident)");
  return 0;
}
