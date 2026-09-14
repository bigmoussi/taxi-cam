#include "code_contract.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace {
using namespace taxi_camera::native_camera;
using taxi_camera::discovery::ImageReader;
using taxi_camera::discovery::Inventory;
using taxi_camera::discovery::ReadWindow;

constexpr std::uint32_t Optional = 0x98;
constexpr std::uint32_t Code = 0x1100;
constexpr std::uint32_t Reloc = 0x30000;

struct SyntheticReader final : ImageReader {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x140000);
  std::vector<std::pair<std::uint32_t, std::uint32_t>> reads;
  std::uint32_t maximum_window = 8192;
  std::uint32_t fail_rva = 0xffffffff;
  std::uint32_t hole_rva = 0xffffffff;
  bool oversized = false;
  bool stalled = false;

  ReadWindow query(std::uint32_t rva, std::uint32_t maximum) override {
    if (stalled || rva >= bytes.size()) {
      return {};
    }
    if (oversized) {
      return {maximum + 1, true};
    }
    auto size = std::min({maximum, maximum_window, static_cast<std::uint32_t>(bytes.size()) - rva});
    if (rva == hole_rva) {
      return {size, false};
    }
    if (rva < hole_rva) {
      size = std::min(size, hole_rva - rva);
    }
    return {size, true};
  }

  bool read(std::uint32_t rva, void* destination, std::size_t size) override {
    reads.emplace_back(rva, static_cast<std::uint32_t>(size));
    if (rva > bytes.size() || size > bytes.size() - rva) {
      return false;
    }
    if (rva == fail_rva) {
      std::memset(destination, 0xff, size / 2);
      return false;
    }
    std::memcpy(destination, bytes.data() + rva, size);
    return true;
  }

  void word(std::uint32_t rva, std::uint32_t value, std::uint32_t size = 4) {
    for (std::uint32_t index = 0; index < size; ++index) {
      bytes.at(rva + index) = static_cast<std::uint8_t>(value >> (index * 8));
    }
  }

  void text(std::uint32_t rva, const char* value) { std::memcpy(bytes.data() + rva, value, std::strlen(value)); }

  void relocation(std::uint16_t entry, std::uint32_t page = 0x1000) {
    word(Optional + 152, Reloc);
    word(Optional + 156, 12);
    word(Reloc, page);
    word(Reloc + 4, 12);
    word(Reloc + 8, entry, 2);
    word(Reloc + 10, 0, 2);
  }
};

Inventory metadata() {
  Inventory image;
  image.valid_image = true;
  image.machine = 0x8664;
  image.timestamp = 4567;
  image.checksum = 1234;
  image.image_size = 0x140000;
  image.section_count = 2;
  image.sections = {{".text", 0x1000, 0x21000, 0x60000020}, {".reloc", Reloc, 0x110000, 0x42000040}};
  return image;
}

SyntheticReader fixture() {
  auto image = metadata();
  SyntheticReader reader;
  reader.word(0, 0x5a4d, 2);
  reader.word(60, 0x80);
  reader.word(0x80, 0x4550);
  reader.word(0x84, image.machine, 2);
  reader.word(0x86, image.section_count, 2);
  reader.word(0x88, image.timestamp);
  reader.word(0x94, 240, 2);
  reader.word(Optional, 0x20b, 2);
  reader.word(Optional + 56, image.image_size);
  reader.word(Optional + 60, 0x400);
  reader.word(Optional + 64, image.checksum);
  reader.word(Optional + 108, 16);
  reader.text(Code, "hello");
  reader.text(Code + 16, "foobar");
  return reader;
}

std::uint32_t checks = 0;

void require(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void budget(const SyntheticReader& reader, const CodeContractInventory& result) {
  std::uint32_t code_bytes = 0;
  std::uint32_t metadata_bytes = 0;
  for (const auto& read : reader.reads) {
    require(read.second != 0 && read.first < reader.bytes.size() && read.second <= reader.bytes.size() - read.first,
            "A code-contract read escaped image bounds");
    if (read.first >= 0x1000 && read.first < 0x22000) {
      code_bytes += read.second;
      require(read.second <= 8192 && read.first + read.second <= 0x22000, "A code request exceeded its range-size limit");
    } else {
      metadata_bytes += read.second;
      require(read.first + read.second <= 0x1000 || read.first >= Reloc, "Metadata escaped headers and relocation storage");
    }
  }
  require(code_bytes <= result.read_bytes && result.read_bytes <= 65536, "Code read accounting or cap failed");
  require(metadata_bytes <= result.metadata_bytes && result.metadata_bytes <= 1024 * 1024, "Metadata read accounting or cap failed");
}

void hash_and_verification() {
  const auto image = metadata();
  const std::vector<CodeFingerprint> expected = {{Code, 5, 0xa430d84680aabd0bull}, {Code + 16, 6, 0x85944171f73967e8ull}};
  for (const auto window : {1u, 3u, 16u, 8192u}) {
    auto reader = fixture();
    reader.maximum_window = window;
    const auto result = verify_code_contract(reader, image, expected);
    require(result.valid && result.relocations_checked && result.error.empty() && result.records.size() == 2 && result.read_bytes == 11,
            "Known FNV-1a vectors or fragmented exact reads failed");
    require(result.records[0].hash == expected[0].hash && result.records[1].hash == expected[1].hash,
            "FNV-1a hashes depend on read-window layout");
    budget(reader, result);
  }
  auto reader = fixture();
  reader.bytes[Code + 2] ^= 1;
  auto result = verify_code_contract(reader, image, expected);
  require(!result.valid && result.records.size() == 2 && result.records[0].hash != expected[0].hash && result.read_bytes == 11,
          "Changed code byte was accepted or remaining required ranges were not sampled");
  budget(reader, result);

  reader = fixture();
  auto wrong = expected;
  wrong[1].hash ^= 1;
  result = verify_code_contract(reader, image, wrong);
  require(!result.valid && result.records.size() == 2, "A later required mismatch was ignored");
  budget(reader, result);

  for (const auto& manifest :
       {std::vector<CodeFingerprint>{}, std::vector<CodeFingerprint>{{Code, 5, 0}}, std::vector<CodeFingerprint>(33, {Code, 5, 1})}) {
    reader = fixture();
    result = verify_code_contract(reader, image, manifest);
    require(!result.valid && reader.reads.empty(), "An invalid or placeholder manifest caused reads");
    budget(reader, result);
  }
}

void range_limits() {
  const auto image = metadata();
  const std::vector<std::vector<CodeRange>> bad = {{},
                                                   {{Code, 0}},
                                                   {{Code, 8193}},
                                                   {{Code, 0xffffffff}},
                                                   {{0xffffffff, 8}},
                                                   {{0, 8}},
                                                   {{0x21ff9, 8}},
                                                   {{Reloc, 8}},
                                                   {{Code, 8}, {Code, 8}},
                                                   {{Code, 8}, {Code + 7, 8}},
                                                   std::vector<CodeRange>(33, {Code, 1})};
  for (const auto& ranges : bad) {
    auto reader = fixture();
    const auto result = inspect_code_contract(reader, image, ranges);
    require(!result.valid && result.records.empty() && reader.reads.empty(), "Invalid ranges caused metadata or code reads");
    budget(reader, result);
  }
  std::vector<CodeRange> maximum;
  for (std::uint32_t index = 0; index < 8; ++index) {
    maximum.push_back({0x1000 + index * 8192, 8192});
  }
  auto reader = fixture();
  auto result = inspect_code_contract(reader, image, maximum);
  require(result.valid && result.read_bytes == 65536 && result.records.size() == 8, "Exact aggregate code limit failed");
  budget(reader, result);

  maximum.push_back({0x11000, 1});
  reader = fixture();
  result = inspect_code_contract(reader, image, maximum);
  require(!result.valid && reader.reads.empty(), "Aggregate code overflow caused reads");
  budget(reader, result);

  maximum.clear();
  for (std::uint32_t index = 0; index < 32; ++index) {
    maximum.push_back({Code + index, 1});
  }
  reader = fixture();
  result = inspect_code_contract(reader, image, maximum);
  require(result.valid && result.records.size() == 32 && result.read_bytes == 32, "Exactly32 adjacent disjoint ranges failed");
  budget(reader, result);

  for (const auto flags : {0xe0000020u, 0x62000020u, 0x40000040u, 0x20000020u}) {
    reader = fixture();
    auto changed = image;
    changed.sections[0].flags = flags;
    result = inspect_code_contract(reader, changed, {{Code, 5}});
    require(!result.valid && reader.reads.empty(), "Excluded code section permission was accepted");
    budget(reader, result);
  }
  reader = fixture();
  auto split = image;
  split.sections[0].size = Code + 2 - 0x1000;
  split.sections.push_back({".text2", Code + 2, 0x22000 - Code - 2, 0x60000020});
  split.section_count = 3;
  result = inspect_code_contract(reader, split, {{Code, 5}});
  require(!result.valid && reader.reads.empty(), "A code range crossed a section boundary");
  budget(reader, result);
}

void header_failures() {
  const auto image = metadata();
  const std::pair<std::uint32_t, std::uint32_t> edits[] = {{0, 0},
                                                           {60, 0xffffffff},
                                                           {60, Code},
                                                           {60, Reloc},
                                                           {60, 0x1000 - 20},
                                                           {0x80, 0},
                                                           {0x84, 0x14c},
                                                           {0x86, 1},
                                                           {0x88, 0},
                                                           {0x94, 111},
                                                           {0x94, 4097},
                                                           {Optional, 0x10b},
                                                           {Optional + 56, 0},
                                                           {Optional + 60, 0},
                                                           {Optional + 60, 0x1001},
                                                           {Optional + 60, 0x100},
                                                           {Optional + 64, 0},
                                                           {Optional + 108, 17}};
  for (const auto& edit : edits) {
    auto reader = fixture();
    const bool half = edit.first == 0 || edit.first == 0x84 || edit.first == 0x86 || edit.first == 0x94 || edit.first == Optional;
    reader.word(edit.first, edit.second, half ? 2 : 4);
    const auto result = inspect_code_contract(reader, image, {{Code, 5}});
    require(!result.valid && result.records.empty() && result.read_bytes == 0, "Malformed or redirected PE header was accepted");
    budget(reader, result);
  }
  for (unsigned int variant = 0; variant < 6; ++variant) {
    auto reader = fixture();
    auto changed = image;
    switch (variant) {
      case 0:
        changed.valid_image = false;
        break;
      case 1:
        changed.machine = 0x14c;
        break;
      case 2:
        changed.sections[1].rva = Code;
        break;
      case 3:
        changed.sections[0].size = 0xffffffff;
        break;
      case 4:
        changed.image_size = 0xffffffff;
        break;
      case 5:
        changed.sections.clear();
        break;
    }
    const auto result = inspect_code_contract(reader, changed, {{Code, 5}});
    require(!result.valid && reader.reads.empty(), "Invalid inventory caused reads");
    budget(reader, result);
  }
}

void relocation_overlap() {
  const auto image = metadata();
  for (const auto type : {3u, 10u}) {
    const auto width = type == 3 ? 4u : 8u;
    for (std::uint32_t offset = 0x100 - width; offset <= 0x105; ++offset) {
      auto reader = fixture();
      reader.relocation(static_cast<std::uint16_t>((type << 12) | offset));
      const auto result = inspect_code_contract(reader, image, {{Code, 5}});
      const bool intersects = offset + width > 0x100 && offset < 0x105;
      require(result.valid != intersects && result.relocations_checked != intersects && result.records.empty() == intersects,
              "Relocated-field overlap or adjacent boundary was misclassified");
      require(!intersects || result.read_bytes == 0, "Relocated code was read before relocation refusal");
      budget(reader, result);
    }
  }
  auto reader = fixture();
  reader.relocation(0x0100);
  auto result = inspect_code_contract(reader, image, {{Code, 5}});
  require(result.valid && result.relocation_entries == 2, "ABSOLUTE padding was treated as a modification");
  budget(reader, result);

  reader = fixture();
  reader.relocation(0xa100, 0x2000);
  result = inspect_code_contract(reader, image, {{Code, 5}});
  require(result.valid && result.relocation_rva == Reloc && result.relocation_size == 12 && result.relocation_blocks_skipped == 1 &&
              result.relocation_entries == 0,
          "Unrelated relocation block was read or refused");
  budget(reader, result);

  // A relocation in the next512-byte entry chunk must still be checked.
  reader = fixture();
  reader.relocation(0);
  reader.word(Optional + 156, 528);
  reader.word(Reloc + 4, 528);
  reader.word(Reloc + 8 + 512, 0xa100, 2);
  result = inspect_code_contract(reader, image, {{Code, 5}});
  require(!result.valid && result.read_bytes == 0 && result.relocation_entries == 257, "Later relocation chunk escaped inspection");
  budget(reader, result);
}

void relocation_malformed_and_caps() {
  const auto image = metadata();
  const std::pair<std::uint32_t, std::uint32_t> edits[] = {{Optional + 152, 0},
                                                           {Optional + 152, Reloc + 1},
                                                           {Optional + 152, Code},
                                                           {Optional + 152, 0xfffffffc},
                                                           {Optional + 156, 0},
                                                           {Optional + 156, 7},
                                                           {Optional + 156, 0xffffffff},
                                                           {Optional + 156, 1024 * 1024},
                                                           {Reloc, 0x1001},
                                                           {Reloc, 0x140000},
                                                           {Reloc + 4, 0},
                                                           {Reloc + 4, 7},
                                                           {Reloc + 4, 9},
                                                           {Reloc + 4, 16}};
  for (const auto& edit : edits) {
    auto reader = fixture();
    reader.relocation(0);
    reader.word(edit.first, edit.second);
    const auto result = inspect_code_contract(reader, image, {{Code, 5}});
    require(!result.valid && result.read_bytes == 0 && !result.relocations_checked, "Malformed relocation directory or block was accepted");
    budget(reader, result);
  }
  for (const auto type : {1u, 2u, 4u, 5u, 6u, 7u, 8u, 9u, 11u, 15u}) {
    auto reader = fixture();
    reader.relocation(static_cast<std::uint16_t>((type << 12) | 0x400), 0x1000);
    const auto result = inspect_code_contract(reader, image, {{Code, 5}});
    require(!result.valid && result.read_bytes == 0, "Unsupported relocation type was ignored");
    budget(reader, result);
  }
  for (const auto flags : {0xc2000040u, 0xe2000040u, 0x02000040u}) {
    auto reader = fixture();
    reader.relocation(0);
    auto changed = image;
    changed.sections[1].flags = flags;
    const auto result = inspect_code_contract(reader, changed, {{Code, 5}});
    require(!result.valid && result.read_bytes == 0, "Writable/unreadable relocation data was accepted");
    budget(reader, result);
  }
  for (const auto flags : {0x60000020u, 0x62000040u}) {
    auto reader = fixture();
    reader.relocation(0);
    auto changed = image;
    changed.sections[1].flags = flags;
    const auto result = inspect_code_contract(reader, changed, {{Code, 5}});
    require(result.valid && result.relocations_checked && result.relocation_entries == 2,
            "An explicit read-only relocation directory was refused solely for executable section storage");
    budget(reader, result);
    reader = fixture();
    reader.relocation(0xa100);
    const auto overlap_result = inspect_code_contract(reader, changed, {{Code, 5}});
    require(!overlap_result.valid && overlap_result.read_bytes == 0, "Relocation overlap in executable-section metadata was bypassed");
    budget(reader, overlap_result);
  }
  auto reader = fixture();
  reader.relocation(0xafff, 0x140000);
  auto result = inspect_code_contract(reader, image, {{Code, 5}});
  require(!result.valid && result.read_bytes == 0, "Out-of-image relocation page was accepted");
  budget(reader, result);

  reader = fixture();
  reader.word(Optional + 108, 5);
  result = inspect_code_contract(reader, image, {{Code, 5}});
  require(result.valid && result.relocations_checked && result.relocation_size == 0, "An absent relocation directory entry failed");
  budget(reader, result);

  // Headers take208B. A directory filling exactly the remaining allowance is
  // valid; one extra block cannot cause a metadata request beyond1MiB.
  reader = fixture();
  constexpr std::uint32_t remaining = 1024 * 1024 - 208;
  reader.word(Optional + 152, Reloc);
  reader.word(Optional + 156, remaining);
  reader.word(Reloc, 0x1000);
  reader.word(Reloc + 4, remaining);
  result = inspect_code_contract(reader, image, {{Code, 5}});
  require(result.valid && result.metadata_bytes == 1024 * 1024 && result.read_bytes == 5, "Exact relocation metadata limit failed");
  budget(reader, result);

  // A larger declared directory can be inspected through its block headers
  // when its payload pages cannot affect the selected ranges.
  reader = fixture();
  reader.word(Optional + 152, Reloc);
  reader.word(Optional + 156, 0x101000);
  reader.word(Reloc, 0x4000);
  reader.word(Reloc + 4, 0x101000);
  result = inspect_code_contract(reader, image, {{Code, 5}});
  require(result.valid && result.metadata_bytes == 216 && result.relocation_blocks == 1 && result.relocation_blocks_skipped == 1 &&
              result.relocation_entries == 0,
          "Unrelated large relocation payload was unnecessarily read");
  budget(reader, result);

  reader = fixture();
  reader.word(Optional + 152, Reloc);
  reader.word(Optional + 156, 0x101000);
  reader.word(Reloc, 0x1000);
  reader.word(Reloc + 4, 0x101000);
  result = inspect_code_contract(reader, image, {{Code, 5}});
  require(!result.valid && result.metadata_bytes <= 1024 * 1024 && result.metadata_bytes > 1024 * 1024 - 512 && result.read_bytes == 0,
          "Relevant large relocation payload bypassed the attempted-byte cap");
  budget(reader, result);

  // A previous page's last DIR64 field overlaps the first seven bytes of a
  // requested next page. Filtering only by matching page would miss this.
  reader = fixture();
  reader.relocation(0xafff, 0x1000);
  result = inspect_code_contract(reader, image, {{0x2000, 1}});
  require(!result.valid && result.relocation_blocks_skipped == 0 && result.read_bytes == 0,
          "Previous-page DIR64 straddler escaped the block filter");
  budget(reader, result);
}

void read_failures() {
  const auto image = metadata();
  for (const auto rva : {0u, 0x80u, Optional, Optional + 152, Reloc, Reloc + 8, Code}) {
    for (const auto query_failure : {false, true}) {
      auto reader = fixture();
      reader.relocation(0);
      (query_failure ? reader.hole_rva : reader.fail_rva) = rva;
      const auto result = inspect_code_contract(reader, image, {{Code, 5}});
      require(!result.valid && result.records.empty() && result.read_failures == 1, "Required read failure was ignored");
      budget(reader, result);
    }
  }
  auto reader = fixture();
  reader.fail_rva = Code + 16;
  auto result = inspect_code_contract(reader, image, {{Code, 5}, {Code + 16, 6}});
  require(!result.valid && result.records.size() == 1 && result.records[0].hash == 0xa430d84680aabd0bull && result.read_bytes == 11,
          "Failed later code read discarded prior evidence or produced a partial hash");
  budget(reader, result);

  for (const auto oversized : {false, true}) {
    reader = fixture();
    reader.oversized = oversized;
    reader.stalled = !oversized;
    result = inspect_code_contract(reader, image, {{Code, 5}});
    require(!result.valid && result.read_failures == 1 && reader.reads.empty(), "Invalid query reached read()");
    budget(reader, result);
  }
}

}  // namespace

int main() {
  try {
    hash_and_verification();
    range_limits();
    header_failures();
    relocation_overlap();
    relocation_malformed_and_caps();
    read_failures();
    std::printf("PASS: %u code fingerprint and relocation guard checks.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
