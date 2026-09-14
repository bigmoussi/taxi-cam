#include "rtti_metadata.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace {
using namespace taxi_camera::discovery;
constexpr std::uint64_t kBase = 0x140000000;
constexpr std::uint32_t kTable = 0x2000;
constexpr std::uint32_t kLocator = 0x2100;
constexpr std::uint32_t kHierarchy = 0x2200;
constexpr std::uint32_t kArray = 0x2300;
constexpr std::uint32_t kDescriptor = 0x2400;
constexpr std::uint32_t kType = 0x8000;
constexpr std::uint32_t kSource = 0x8040;
constexpr std::uint32_t kTarget = 0x8080;
std::uint32_t checks = 0;

void require(bool condition, const char* message) {
  ++checks;
  if (!condition)
    throw std::runtime_error(message);
}

struct Reader final : ImageReader {
  std::map<std::uint32_t, std::uint8_t> bytes;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> reads;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> queries;
  std::uint32_t fail_rva = 0;
  std::uint32_t hole_rva = 0;
  std::uint32_t query_chunk = 256;
  bool oversized_query = false;

  ReadWindow query(std::uint32_t rva, std::uint32_t maximum) override {
    queries.emplace_back(rva, maximum);
    if (oversized_query)
      return {maximum + 1, true};
    if (hole_rva >= rva && hole_rva - rva < maximum)
      return {maximum, false};
    return {std::min(maximum, query_chunk), true};
  }

  bool read(std::uint32_t rva, void* destination, std::size_t size) override {
    reads.emplace_back(rva, static_cast<std::uint32_t>(size));
    if (rva == fail_rva)
      return false;
    auto* output = static_cast<std::uint8_t*>(destination);
    for (std::uint32_t index = 0; index < size; ++index) {
      const auto found = bytes.find(rva + index);
      output[index] = found == bytes.end() ? 0 : found->second;
    }
    return true;
  }

  void word(std::uint32_t rva, std::uint64_t value, std::uint32_t size = 4) {
    for (std::uint32_t index = 0; index < size; ++index)
      bytes[rva + index] = static_cast<std::uint8_t>(value >> (index * 8));
  }
};

struct Fixture {
  Reader reader;
  Inventory image;
  std::uint64_t base = kBase;
  std::uint32_t table = kTable;
  std::uint32_t source = kSource;
  std::uint32_t target = kTarget;

  Fixture() {
    image.valid_image = true;
    image.machine = 0x8664;
    image.image_size = 0x10000;
    image.sections = {{".rdata", 0x1000, 0x6000, 0x40000040}, {".data", 0x8000, 0x3000, 0xc0000040}, {".text", 0xc000, 0x2000, 0x60000020}};
    reader.word(kTable - 8, kBase + kLocator, 8);
    reader.word(kLocator, 1);
    reader.word(kLocator + 4, 24);
    reader.word(kLocator + 8, 4);
    reader.word(kLocator + 12, kType);
    reader.word(kLocator + 16, kHierarchy);
    reader.word(kLocator + 20, kLocator);
    graph(3);
  }

  void graph(std::uint32_t count) {
    reader.word(kHierarchy, 0);
    reader.word(kHierarchy + 4, 1);
    reader.word(kHierarchy + 8, count);
    reader.word(kHierarchy + 12, kArray);
    for (std::uint32_t index = 0; index < count; ++index) {
      const auto descriptor = kDescriptor + index * 32;
      reader.word(kArray + index * 4, descriptor);
      reader.word(descriptor, index == 0 ? kType : index == 1 ? kSource : kTarget);
      reader.word(descriptor + 4, count - index - 1);
      reader.word(descriptor + 8, index * 24);
      reader.word(descriptor + 12, 0xffffffff);
      reader.word(descriptor + 16, 0);
      reader.word(descriptor + 20, 64);
      reader.word(descriptor + 24, kHierarchy);
    }
  }

  RttiMetadata run() {
    reader.reads.clear();
    reader.queries.clear();
    auto result = inspect_rtti_metadata(reader, image, base, table, source, target);
    require(result.read_bytes <= 4096 && result.read_bytes <= 2096 && result.bases.size() <= 64, "RTTI aggregate cap exceeded");
    std::uint32_t read_bytes = 0;
    for (const auto& read : reader.reads) {
      require(read.second > 0 && read.second <= 256 && read.first % 4 == 0 && std::uint64_t(read.first) + read.second <= image.image_size,
              "RTTI read exceeded a word/descriptor/array or image boundary");
      const auto static_data = std::any_of(image.sections.begin(), image.sections.end(), [&](const auto& section) {
        return (section.flags & 0x40000000) != 0 && (section.flags & (0x80000000 | 0x20000000 | 0x02000000)) == 0 &&
               read.first >= section.rva && std::uint64_t(read.first) + read.second <= std::uint64_t(section.rva) + section.size;
      });
      require(static_data, "RTTI reader accessed writable, executable or undeclared storage");
      read_bytes += read.second;
    }
    require(result.read_bytes == read_bytes, "RTTI attempted read-byte accounting is inaccurate");
    for (const auto& query : reader.queries) {
      require(query.first < kType || query.first >= 0xb000, "A TypeDescriptor header/name was queried");
      require(query.second > 0 && query.second <= 256 && std::uint64_t(query.first) + query.second <= image.image_size,
              "RTTI query escaped its bounded metadata range");
    }
    if (result.valid)
      require(result.error.empty() && result.stage == "complete" && result.bases.size() == result.base_count && result.read_failures == 0,
              "Valid RTTI result retained a failure or incomplete graph");
    return result;
  }
};

void reject_without_access(Fixture& test) {
  const auto result = test.run();
  require(!result.valid && !result.error.empty() && test.reader.reads.empty() && test.reader.queries.empty(),
          "Invalid input metadata was not refused before reader access");
}

void basic_graph_and_numeric_fields() {
  Fixture test;
  auto result = test.run();
  require(result.valid && result.read_bytes == 144 && result.vtable_rva == kTable && result.locator_rva == kLocator &&
              result.locator_signature == 1 && result.offset == 24 && result.cd_offset == 4 && result.type_rva == kType &&
              result.hierarchy_rva == kHierarchy && result.self_rva == kLocator && result.hierarchy_signature == 0 &&
              result.hierarchy_attributes == 1 && result.base_count == 3 && result.base_array_rva == kArray && result.source_type_present &&
              result.target_type_present && result.source_type_rva == kSource && result.target_type_rva == kTarget,
          "Expected numeric RTTI graph did not round-trip");
  const std::vector<std::pair<std::uint32_t, std::uint32_t>> expected{
      {kTable - 8, 8}, {kLocator, 24}, {kHierarchy, 16}, {kArray, 12}, {kDescriptor, 28}, {kDescriptor + 32, 28}, {kDescriptor + 64, 28}};
  require(test.reader.reads == expected, "Reader accessed more than the requested one-level graph");
  require(result.bases[1].index == 1 && result.bases[1].descriptor_rva == kDescriptor + 32 && result.bases[1].type_rva == kSource &&
              result.bases[1].num_contained_bases == 1 && result.bases[1].mdisp == 24 && result.bases[1].pdisp == -1 &&
              result.bases[1].vdisp == 0 && result.bases[1].attributes == 64 && result.bases[1].hierarchy_rva == kHierarchy,
          "Base descriptor field offsets or signed PMDs are wrong");
  test.reader.word(kDescriptor + 32 + 8, 0x80000000);
  test.reader.word(kDescriptor + 32 + 12, 16);
  test.reader.word(kDescriptor + 32 + 16, 0xffffffff);
  test.reader.word(kDescriptor + 32 + 20, 0xffffffff);
  result = test.run();
  require(result.valid && result.bases[1].mdisp == std::numeric_limits<std::int32_t>::min() && result.bases[1].pdisp == 16 &&
              result.bases[1].vdisp == -1 && result.bases[1].attributes == 0xffffffff,
          "Numeric virtual/private/unknown attributes or PMDs were interpreted as a cast rule");
  test = Fixture{};
  test.reader.word(kDescriptor + 64 + 20, 0);
  test.reader.word(kDescriptor + 64 + 24, 0);
  require(test.run().valid, "Absent optional hierarchy reference without the presence bit was refused");
  test = Fixture{};
  test.source = test.target = kType;
  result = test.run();
  require(result.valid && result.source_type_present && result.target_type_present,
          "Equal numeric source/target identities were mishandled");
  test.source = 0x80c0;
  test.target = 0x8100;
  result = test.run();
  require(result.valid && !result.source_type_present && !result.target_type_present,
          "Absent types invalidated or falsely matched the graph");
}

void input_refusals() {
  for (const auto base : {std::uint64_t(0), kBase + 1, std::numeric_limits<std::uint64_t>::max() - 7}) {
    Fixture test;
    test.base = base;
    reject_without_access(test);
  }
  for (const auto table : {0u, 7u, 0x1000u, kTable + 1, 0x6ff8u + 8, 0x8000u, 0xc000u, 0xfffffff8u}) {
    Fixture test;
    test.table = table;
    reject_without_access(test);
  }
  for (const auto type : {0u, kSource + 1, 0x7000u, 0xaff8u, 0xc000u, 0xfffffff8u}) {
    Fixture test;
    test.source = type;
    reject_without_access(test);
    test = Fixture{};
    test.target = type;
    reject_without_access(test);
  }
  for (unsigned mutation = 0; mutation < 11; ++mutation) {
    Fixture test;
    switch (mutation) {
      case 0:
        test.image.valid_image = false;
        break;
      case 1:
        test.image.machine = 0x14c;
        break;
      case 2:
        test.image.image_size = 0;
        break;
      case 3:
        test.image.sections.clear();
        break;
      case 4:
        test.image.sections.resize(97);
        break;
      case 5:
        test.image.sections[0].size = 0xffffffff;
        break;
      case 6:
        test.image.sections[0].rva = 0xffffffff;
        break;
      case 7:
        test.image.sections[1].rva = kTable;
        break;
      case 8:
        test.image.sections[0].flags |= 0x80000000;
        break;
      case 9:
        test.image.sections[0].flags |= 0x02000000;
        break;
      case 10:
        test.image.sections[1].flags = 0;
        break;
    }
    reject_without_access(test);
  }
}

void locator_and_hierarchy_refusals() {
  for (const auto pointer : {std::uint64_t(0), kBase - 8, kBase + 0x10000, std::numeric_limits<std::uint64_t>::max()}) {
    Fixture test;
    test.reader.word(kTable - 8, pointer, 8);
    const auto result = test.run();
    require(!result.valid && result.stage == "locator_pointer" && result.read_bytes == 8, "Foreign locator pointer was followed");
  }
  for (const auto pointer : {kBase + kLocator + 1, kBase + 0x6ff0, kBase + 0x8000, kBase + 0xc000}) {
    Fixture test;
    test.reader.word(kTable - 8, pointer, 8);
    const auto result = test.run();
    require(!result.valid && result.stage == "locator" && result.read_bytes == 8, "Ineligible or clipped locator storage was read");
  }
  for (const auto field : {kLocator, kLocator + 12, kLocator + 20}) {
    Fixture test;
    test.reader.word(field, 0);
    const auto result = test.run();
    require(!result.valid && result.stage == "locator" && result.read_bytes == 32, "Invalid locator signature/type/self was accepted");
  }
  Fixture test;
  test.base += 0x100;
  auto result = test.run();
  require(!result.valid, "A changed image base survived locator identity validation");
  test = Fixture{};
  test.reader.word(kLocator + 20, kLocator + 4);
  result = test.run();
  require(!result.valid && result.read_bytes == 32, "Mismatched locator self-RVA was accepted");
  for (const auto rva : {0u, kHierarchy + 1, 0x6ff4u, 0x8000u, 0xc000u, 0xfffffffcu}) {
    test = Fixture{};
    test.reader.word(kLocator + 16, rva);
    result = test.run();
    require(!result.valid && result.stage == "hierarchy" && result.read_bytes == 32, "Ineligible hierarchy storage was read");
  }
  for (const auto count : {0u, 65u, 0xffffffffu}) {
    test = Fixture{};
    test.reader.word(kHierarchy + 8, count);
    result = test.run();
    require(!result.valid && result.stage == "hierarchy" && result.read_bytes == 48, "Unbounded hierarchy count caused array reads");
  }
  test = Fixture{};
  test.reader.word(kHierarchy, 1);
  result = test.run();
  require(!result.valid && result.read_bytes == 48, "Unknown hierarchy signature was accepted");
  for (const auto rva : {0u, kArray + 1, 0x6ff8u, 0x8000u, 0xc000u, 0xfffffffcu}) {
    test = Fixture{};
    test.reader.word(kHierarchy + 12, rva);
    result = test.run();
    require(!result.valid && result.stage == "base_array" && result.read_bytes == 48, "Ineligible or truncated base array was read");
  }
}

void base_descriptor_refusals_and_partial_results() {
  for (const auto rva : {0u, kDescriptor + 1, 0x6fe8u, 0x8000u, 0xc000u, 0xfffffffcu}) {
    Fixture test;
    test.reader.word(kArray, rva);
    const auto result = test.run();
    require(!result.valid && result.stage == "base_descriptor" && result.read_bytes == 60 && result.bases.empty(),
            "Ineligible or truncated base descriptor was read");
  }
  for (unsigned mutation = 0; mutation < 7; ++mutation) {
    Fixture test;
    const auto descriptor = kDescriptor + 64;
    switch (mutation) {
      case 0:
        test.reader.word(descriptor, 0);
        break;
      case 1:
        test.reader.word(descriptor + 4, 1);
        break;
      case 2:
        test.reader.word(descriptor + 24, 0);
        break;
      case 3:
        test.reader.word(descriptor + 24, 0x8000);
        break;
      case 4:
        test.reader.word(descriptor + 24, kHierarchy + 1);
        break;
      case 5:
        test.reader.word(descriptor + 24, 0xc000);
        break;
      case 6:
        test.reader.word(descriptor + 24, 0x6ff4);
        break;
    }
    const auto result = test.run();
    require(!result.valid && !result.error.empty() && result.read_bytes == 144 && result.bases.size() == 2 && result.source_type_present &&
                !result.target_type_present,
            "Failed final descriptor invalidated earlier numeric evidence or reported target presence");
  }
  Fixture test;
  test.reader.word(kDescriptor, kSource);
  auto result = test.run();
  require(!result.valid && result.read_bytes == 88 && result.bases.empty(), "Root descriptor identity did not match the locator");
  test = Fixture{};
  test.reader.word(kDescriptor + 4, 1);
  result = test.run();
  require(!result.valid && result.bases.empty(), "Root contained count did not match the full hierarchy");
}

void read_failures_and_caps() {
  Fixture baseline;
  require(baseline.run().valid, "Failure fixture graph was invalid");
  const auto original_reads = baseline.reader.reads;
  std::uint32_t earlier_bytes = 0;
  for (const auto& read : original_reads) {
    Fixture test;
    test.reader.fail_rva = read.first;
    auto result = test.run();
    require(!result.valid && result.read_failures == 1 && result.read_bytes == earlier_bytes + read.second,
            "Failed exact RTTI read accounting is inaccurate");
    test = Fixture{};
    test.reader.hole_rva = read.first + read.second - 1;
    result = test.run();
    require(!result.valid && result.read_failures == 1 && result.read_bytes == earlier_bytes,
            "Unreadable final byte allowed a read or consumed attempted-read bytes");
    earlier_bytes += read.second;
  }
  Fixture test;
  test.reader.query_chunk = 1;
  auto result = test.run();
  require(result.valid && test.reader.queries.size() == 144, "Fragmented readable metadata windows were refused");
  test.reader.query_chunk = 0;
  result = test.run();
  require(!result.valid && result.read_bytes == 0 && test.reader.queries.size() == 1, "Zero-progress query did not stop");
  test.reader.oversized_query = true;
  result = test.run();
  require(!result.valid && result.read_bytes == 0, "An oversized reader query was accepted");
  for (std::uint32_t count = 1; count <= 64; ++count) {
    test = Fixture{};
    test.graph(count);
    result = test.run();
    require(result.valid && result.base_count == count && result.bases.size() == count && result.read_bytes == 48 + count * 32,
            "A permitted graph exceeded or incorrectly consumed its bounded one-level reads");
  }
  require(result.read_bytes == 2096 && result.read_bytes <= 4096, "Maximum graph did not respect the hard attempted-byte budget");
}

RttiMetadata component_profile() {
  RttiMetadata metadata;
  metadata.valid = true;
  metadata.stage = "complete";
  metadata.read_bytes = 240;
  metadata.vtable_rva = 133516648;
  metadata.locator_rva = metadata.self_rva = 146401552;
  metadata.locator_signature = 1;
  metadata.type_rva = metadata.target_type_rva = 165937384;
  metadata.hierarchy_rva = 146401592;
  metadata.base_count = 6;
  // The saved JSON did not emit the array address. This aligned synthetic
  // address is only for testing the gate's requirement for prior valid metadata.
  metadata.base_array_rva = 146401616;
  metadata.source_type_rva = 166129704;
  metadata.source_type_present = metadata.target_type_present = true;
  metadata.bases = {{0, 146401672, 165937384, 5, 0, -1, 0, 64, 146401592}, {1, 145841576, 166625656, 4, 0, -1, 0, 64, 145841504},
                    {2, 145185440, 166129704, 3, 0, -1, 0, 64, 145185376}, {3, 145185192, 166129616, 2, 0, -1, 0, 64, 145185232},
                    {4, 145185288, 166129664, 1, 0, -1, 0, 64, 145185328}, {5, 145130208, 166092480, 0, 0, -1, 0, 64, 145130248}};
  return metadata;
}

void camera_component_profile() {
  const auto expected = component_profile();
  require(verified_camera_component_layout(expected), "The fixed observed numeric component profile was refused");
  require(!verified_camera_component_layout(RttiMetadata{}), "Absent metadata passed the fixed-profile gate");
  Fixture other;
  require(!verified_camera_component_layout(other.run()), "Another valid hierarchy passed the build-specific component profile");
  for (unsigned mutation = 0; mutation < 24; ++mutation) {
    auto metadata = expected;
    switch (mutation) {
      case 0:
        metadata.valid = false;
        break;
      case 1:
        metadata.stage = "base_descriptor";
        break;
      case 2:
        metadata.error = "partial";
        break;
      case 3:
        metadata.read_failures = 1;
        break;
      case 4:
        metadata.read_bytes = 239;
        break;
      case 5:
        ++metadata.vtable_rva;
        break;
      case 6:
        ++metadata.locator_rva;
        break;
      case 7:
        metadata.locator_signature = 0;
        break;
      case 8:
        metadata.offset = 8;
        break;
      case 9:
        metadata.cd_offset = 4;
        break;
      case 10:
        metadata.type_rva = metadata.source_type_rva;
        break;
      case 11:
        ++metadata.hierarchy_rva;
        break;
      case 12:
        ++metadata.self_rva;
        break;
      case 13:
        metadata.hierarchy_signature = 1;
        break;
      case 14:
        metadata.hierarchy_attributes = 1;
        break;
      case 15:
        metadata.base_count = 5;
        break;
      case 16:
        metadata.bases.pop_back();
        break;
      case 17:
        metadata.bases.push_back(metadata.bases.back());
        break;
      case 18:
        metadata.base_array_rva = 0;
        break;
      case 19:
        ++metadata.base_array_rva;
        break;
      case 20:
        ++metadata.source_type_rva;
        break;
      case 21:
        ++metadata.target_type_rva;
        break;
      case 22:
        metadata.source_type_present = false;
        break;
      case 23:
        metadata.target_type_present = false;
        break;
    }
    require(!verified_camera_component_layout(metadata), "Changed/incomplete numeric RTTI identity passed the fixed-profile gate");
  }
  for (std::size_t index = 0; index < expected.bases.size(); ++index) {
    for (unsigned mutation = 0; mutation < 11; ++mutation) {
      auto metadata = expected;
      auto& entry = metadata.bases[index];
      switch (mutation) {
        case 0:
          ++entry.index;
          break;
        case 1:
          ++entry.descriptor_rva;
          break;
        case 2:
          entry.type_rva = 0;
          break;
        case 3:
          ++entry.num_contained_bases;
          break;
        case 4:
          entry.mdisp = 8;
          break;
        case 5:
          entry.pdisp = 0;
          break;
        case 6:
          entry.vdisp = 4;
          break;
        case 7:
          entry.attributes |= 1;
          break;
        case 8:
          entry.attributes |= 2;
          break;
        case 9:
          entry.attributes |= 16;
          break;
        case 10:
          ++entry.hierarchy_rva;
          break;
      }
      require(!verified_camera_component_layout(metadata),
              "Changed base identity, visibility/ambiguity flag or PMD passed the fixed profile");
    }
  }
  auto metadata = expected;
  metadata.bases[1].type_rva = metadata.source_type_rva;
  require(!verified_camera_component_layout(metadata), "Duplicate source type passed the unique chain profile");
  metadata = expected;
  metadata.bases[4].type_rva = metadata.target_type_rva;
  require(!verified_camera_component_layout(metadata), "Duplicate target type passed the unique chain profile");
  metadata = expected;
  std::swap(metadata.bases[1], metadata.bases[2]);
  require(!verified_camera_component_layout(metadata), "Reordered bases passed the exact chain profile");
  metadata = expected;
  metadata.bases[0].num_contained_bases = 4;
  metadata.bases[1].num_contained_bases = 3;
  require(!verified_camera_component_layout(metadata), "Changed chain topology passed the exact numeric profile");
}
}  // namespace

int main() {
  try {
    basic_graph_and_numeric_fields();
    input_refusals();
    locator_and_hierarchy_refusals();
    base_descriptor_refusals_and_partial_results();
    read_failures_and_caps();
    camera_component_profile();
    std::printf("PASS: %u numeric RTTI checks; no type-name, object, code or process reads.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
