#include "../../src/camera/activation_mask.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
using namespace taxi_camera;
constexpr std::uint32_t kRva = 130434096;
unsigned checks = 0;
void require(bool value, const char* error) {
  ++checks;
  if (!value)
    throw std::runtime_error(error);
}
struct Reader final : discovery::ImageReader {
  std::array<std::uint8_t, 16> bytes{1};
  unsigned reads = 0, queries = 0, read_bytes = 0, window = 16;
  bool inaccessible = false, fail = false, change = false, oversized = false;
  discovery::ReadWindow query(std::uint32_t rva, std::uint32_t maximum) override {
    ++queries;
    require(rva >= kRva && rva - kRva < 16 && maximum <= 16 - (rva - kRva), "Query escaped the fixed pair");
    return {oversized ? maximum + 1 : std::min(window, maximum), !inaccessible};
  }
  bool read(std::uint32_t rva, void* destination, std::size_t size) override {
    ++reads;
    require(rva >= kRva && rva - kRva < 16 && size <= 16 - (rva - kRva), "Read escaped the fixed pair");
    if (change && read_bytes == 16)
      bytes[0] ^= 2;
    read_bytes += static_cast<unsigned>(size);
    if (fail)
      return false;
    std::memcpy(destination, bytes.data() + (rva - kRva), size);
    return true;
  }
};
discovery::Inventory image() {
  discovery::Inventory result;
  result.valid_image = true;
  result.machine = 0x8664;
  result.timestamp = 1787653788;
  result.image_size = 235963904;
  result.section_count = 14;
  result.sections = {{"data", kRva - 32, 64, 0x40000040}};
  return result;
}
void tests() {
  {
    Reader reader;
    auto metadata = image();
    ++metadata.timestamp;
    metadata.image_size += 4096;
    ++metadata.section_count;
    require(native_camera::inspect_activation_disable_mask(reader, metadata).valid,
            "Compatible activation data was rejected solely for changed build metadata");
  }
  for (unsigned window = 1; window <= 16; ++window) {
    Reader reader;
    reader.window = window;
    const auto result = native_camera::inspect_activation_disable_mask(reader, image());
    require(result.valid && result.words == std::array<std::uint64_t, 2>{1, 0} && result.read_bytes == 32 && result.read_failures == 0 &&
                reader.read_bytes == 32,
            "Fragmented mask verification failed");
  }
  for (unsigned byte = 0; byte < 16; ++byte) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      Reader reader;
      reader.bytes[byte] ^= static_cast<std::uint8_t>(1u << bit);
      const auto result = native_camera::inspect_activation_disable_mask(reader, image());
      require(!result.valid && result.read_bytes == 32 && result.read_failures == 0, "Unexpected mask bit was accepted");
    }
  }
  for (unsigned failure = 0; failure < 5; ++failure) {
    Reader reader;
    reader.inaccessible = failure == 0;
    reader.fail = failure == 1;
    reader.change = failure == 2;
    reader.oversized = failure == 3;
    reader.window = failure == 4 ? 0 : 16;
    const auto result = native_camera::inspect_activation_disable_mask(reader, image());
    require(!result.valid && result.read_bytes <= 32 && !result.error.empty(), "Unreadable/changed/malformed mask was accepted");
  }
  for (unsigned failure = 0; failure < 12; ++failure) {
    Reader reader;
    auto metadata = image();
    if (failure == 0)
      metadata.valid_image = false;
    if (failure == 1)
      metadata.machine = 0;
    if (failure == 2)
      metadata.section_count = 97;
    if (failure == 3)
      metadata.image_size = 0;
    if (failure == 4)
      metadata.section_count = 0;
    if (failure == 5)
      metadata.sections.clear();
    if (failure == 6)
      metadata.sections[0].size = 47;
    if (failure == 7)
      metadata.sections[0].rva = 0xffffffff;
    if (failure == 8)
      metadata.sections[0].flags |= 0x80000000;
    if (failure == 9)
      metadata.sections[0].flags |= 0x20000000;
    if (failure == 10)
      metadata.sections[0].flags |= 0x02000000;
    if (failure == 11)
      metadata.sections[0].flags = 0;
    const auto result = native_camera::inspect_activation_disable_mask(reader, metadata);
    require(!result.valid && result.read_bytes == 0 && reader.queries == 0 && reader.reads == 0, "Invalid image metadata caused reads");
  }
}
}  // namespace
int main() {
  try {
    tests();
    std::printf("PASS: %u fixed activation-mask checks; 32-byte maximum, no engine calls.\n", checks);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
