#include "../../tools/diagnostics/guard_inventory.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
using namespace taxi_camera::discovery;
constexpr std::uint64_t Base = 0x140000000;
constexpr std::uint32_t Config = 0x2200;
constexpr std::uint32_t Slot = 0x2300;

struct Reader final : ImageReader {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x6000);
  std::vector<std::pair<std::uint32_t, std::uint32_t>> reads;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> holes;
  bool fail_reads = false;
  ReadWindow query(std::uint32_t rva, std::uint32_t maximum) override {
    if (rva >= bytes.size())
      return {};
    return {std::min(maximum, static_cast<std::uint32_t>(bytes.size()) - rva), true};
  }
  bool read(std::uint32_t rva, void* destination, std::size_t size) override {
    reads.emplace_back(rva, static_cast<std::uint32_t>(size));
    if (fail_reads || rva >= bytes.size() || size > bytes.size() - rva)
      return false;
    for (const auto& hole : holes) {
      if (rva < hole.second && static_cast<std::uint64_t>(rva) + size > hole.first)
        return false;
    }
    std::memcpy(destination, bytes.data() + rva, size);
    return true;
  }
  void word(std::uint32_t offset, std::uint16_t value) {
    bytes.at(offset) = static_cast<std::uint8_t>(value);
    bytes.at(offset + 1) = static_cast<std::uint8_t>(value >> 8);
  }
  void dword(std::uint32_t offset, std::uint32_t value) {
    word(offset, static_cast<std::uint16_t>(value));
    word(offset + 2, static_cast<std::uint16_t>(value >> 16));
  }
  void qword(std::uint32_t offset, std::uint64_t value) {
    dword(offset, static_cast<std::uint32_t>(value));
    dword(offset + 4, static_cast<std::uint32_t>(value >> 32));
  }
};

Reader fixture() {
  Reader reader;
  reader.word(0, 0x5a4d);
  reader.dword(60, 0x80);
  reader.dword(0x80, 0x00004550);
  reader.word(0x84, 0x8664);
  reader.word(0x86, 3);
  reader.word(0x94, 0xf0);
  reader.word(0x98, 0x20b);
  reader.dword(0xd0, 0x6000);
  reader.dword(0xd4, 0x400);
  reader.word(0xde, 0x4000);
  reader.dword(0x104, 16);
  reader.dword(0x158, Config);
  reader.dword(0x15c, 148);
  const std::uint32_t flags[] = {0x60000020, 0x40000040, 0xc0000040};
  const std::uint32_t rvas[] = {0x1000, 0x2000, 0x4000};
  const std::uint32_t sizes[] = {0x1000, 0x2000, 0x1000};
  for (std::uint32_t i = 0; i < 3; ++i) {
    const auto offset = 0x188 + i * 40;
    reader.dword(offset + 8, sizes[i]);
    reader.dword(offset + 12, rvas[i]);
    reader.dword(offset + 36, flags[i]);
  }
  reader.dword(Config, 148);
  reader.qword(Config + 120, Base + Slot);
  reader.dword(Config + 144, 0x500);
  return reader;
}

void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

Inventory parse(Reader& reader) {
  Limits limits;
  limits.scan_bytes = 0;
  auto image = inspect_image(reader, limits);
  require(image.valid_image, "Synthetic PE fixture failed validation");
  reader.reads.clear();
  return image;
}
}  // namespace

int main() {
  try {
    auto reader = fixture();
    const auto image = parse(reader);
    require(image.load_config_rva == Config && image.load_config_size == 148 && image.dll_characteristics == 0x4000,
            "Load-config directory or DLL characteristic parsing failed");
    auto result = inspect_guard_metadata(reader, image, Base);
    require(result.present && result.valid && result.error.empty() && result.guard_cf_characteristic && result.cf_instrumented &&
                result.function_table_present && result.guard_flags == 0x500 && result.dispatch_slot_present &&
                result.dispatch_slot_rva == Slot && result.declared_size == 148 && result.read_bytes == 148 && result.read_failures == 0 &&
                reader.reads == std::vector<std::pair<std::uint32_t, std::uint32_t>>{{Config, 148}},
            "Valid CFG metadata, normalization or exact bounded read failed");
    // A pointer-slot hole must not matter: slot contents are NEVER read.
    reader.holes = {{Slot, Slot + 8}};
    require(inspect_guard_metadata(reader, image, Base).valid, "Dispatch slot contents were accessed");
    reader.holes = {{Config + 148, Config + 256}};
    require(inspect_guard_metadata(reader, image, Base).valid, "Read past the 148-byte load-config prefix");
    reader.holes = {{Config + 120, Config + 128}};
    result = inspect_guard_metadata(reader, image, Base);
    require(!result.valid && result.read_failures == 1 && result.read_bytes == 148, "Unreadable load-config prefix was accepted");
    reader.holes.clear();

    auto changed = image;
    changed.load_config_rva = changed.load_config_size = 0;
    result = inspect_guard_metadata(reader, changed, Base);
    require(result.valid && !result.present && result.read_bytes == 0 && !result.dispatch_slot_present,
            "Absent directory was misclassified");
    for (const auto rva : {0u, 0x1000u, 0x4000u, 0x5fffu, 0xfffffff0u}) {
      changed = image;
      changed.load_config_rva = rva;
      result = inspect_guard_metadata(reader, changed, Base);
      require(result.present && !result.valid && result.read_bytes == 0, "Invalid or non-static directory location was accepted");
    }
    for (const auto size : {0u, 147u, 0xffffffffu}) {
      changed = image;
      changed.load_config_size = size;
      result = inspect_guard_metadata(reader, changed, Base);
      require(result.present && !result.valid && result.read_bytes == 0, "Truncated or overflowed directory extent was accepted");
    }
    for (const auto declared : {0u, 147u, 149u, 0xffffffffu}) {
      reader.dword(Config, declared);
      result = inspect_guard_metadata(reader, image, Base);
      require(!result.valid && result.read_bytes == 148 && result.declared_size == declared,
              "Invalid declared structure size was accepted");
    }
    reader.dword(Config, 148);
    for (const auto slot :
         {Base - 8, Base + image.image_size, Base + 0x1000, Base + 0x4000, Base + 0x3ffc, std::numeric_limits<std::uint64_t>::max()}) {
      reader.qword(Config + 120, slot);
      result = inspect_guard_metadata(reader, image, Base);
      require(!result.valid && !result.dispatch_slot_present && result.dispatch_slot_rva == 0,
              "Invalid, stale or non-static pointer-slot address was accepted");
    }
    reader.qword(Config + 120, Base + Slot);
    require(!inspect_guard_metadata(reader, image, Base + 0x100000).valid, "A stale loaded image base was accepted");
    require(!inspect_guard_metadata(reader, image, 0).valid, "A null loaded image base was accepted");
    require(!inspect_guard_metadata(reader, image, std::numeric_limits<std::uint64_t>::max() - image.image_size + 1).valid,
            "Overflowed loaded-image bounds were accepted");
    changed = image;
    changed.valid_image = false;
    require(!inspect_guard_metadata(reader, changed, Base).valid, "An invalid image was accepted");
    changed = image;
    changed.machine = 0x14c;
    require(!inspect_guard_metadata(reader, changed, Base).valid, "A non-AMD64 image was accepted");
    for (const auto flags : {0u, 0x100u, 0x400u, 0x500u, 0xf0000500u}) {
      reader.dword(Config + 144, flags);
      result = inspect_guard_metadata(reader, image, Base);
      require(result.valid && result.guard_flags == flags && result.cf_instrumented == ((flags & 0x100) != 0) &&
                  result.function_table_present == ((flags & 0x400) != 0),
              "Guard flags were lost or confused with metadata validity");
    }
    reader.qword(Config + 120, 0);
    changed = image;
    changed.dll_characteristics = 0;
    result = inspect_guard_metadata(reader, changed, Base);
    require(result.valid && result.present && !result.guard_cf_characteristic && !result.dispatch_slot_present,
            "Absent optional dispatch facility was treated as malformed");
    reader.fail_reads = true;
    result = inspect_guard_metadata(reader, image, Base);
    require(!result.valid && result.read_failures == 1, "A failed memory read was accepted");
    reader = fixture();
    reader.dword(0x104, 10);  // Bytes outside the declared directory count are ignored.
    result = inspect_guard_metadata(reader, parse(reader), Base);
    require(result.valid && !result.present, "Load-config fields were read beyond NumberOfRvaAndSizes");
    std::puts("PASS: bounded CFG load-config parsing, absence, flags, slot normalization, malformed bounds and no slot-content reads.");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
