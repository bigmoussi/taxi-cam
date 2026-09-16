#include "../../src/camera/rtti_vtables.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>

namespace {
using namespace taxi_camera;
using namespace native_camera;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
struct Fixture : discovery::ImageReader {
  std::uint64_t base;
  std::uint32_t delta, col, table, type, hierarchy, code;
  discovery::Inventory image;
  std::vector<std::uint8_t> bytes;
  std::map<std::uint32_t, unsigned> reads;
  std::uint32_t window = 32768, hole_begin = 0, hole_end = 0, change_at = UINT32_MAX, fail_at = UINT32_MAX;
  bool oversized = false;
  std::uint64_t attempted = 0;
  static constexpr const char* Name = ".?AVSceneFixture@@";
  explicit Fixture(std::uint32_t shift = 0, std::uint64_t loaded = 0x140000000)
      : base(loaded),
        delta(shift),
        col(0xbff4 + shift),
        table(0xc108 + shift),
        type(0x16040 + shift),
        hierarchy(0x5000 + shift),
        code(0x1000 + shift),
        bytes(0x20000 + shift) {
    image.valid_image = true;
    image.machine = 0x8664;
    image.image_size = static_cast<std::uint32_t>(bytes.size());
    image.section_count = 3;
    image.sections = {{".text", code, 0x1000, 0x60000020},
                      {".rdata", 0x4000 + shift, 0x11000, 0x40000040},
                      {".data", 0x16000 + shift, 0x1000, 0xc0000040}};
    put(col, 1);
    put(col + 12, type);
    put(col + 16, hierarchy);
    put(col + 20, col);
    put(type, 0x700001234000ull, 8);  // Metadata identity only: never follow this external pointer.
    std::memcpy(bytes.data() + type + 16, Name, std::strlen(Name) + 1);
    put(hierarchy + 8, 1);
    put(hierarchy + 12, hierarchy + 32);
    put(hierarchy + 32, hierarchy + 64);
    put(hierarchy + 64, type);
    put(hierarchy + 64 + 12, UINT32_MAX);
    put(hierarchy + 64 + 20, 64);
    put(hierarchy + 64 + 24, hierarchy);
    put(table - 8, base + col, 8);
    for (unsigned i = 0; i < 4; ++i)
      put(table + i * 8, base + code + 0x100 + i * 16, 8);
  }
  void put(std::uint32_t at, std::uint64_t value, unsigned size = 4) {
    for (unsigned i = 0; i < size; ++i)
      bytes.at(at + i) = static_cast<std::uint8_t>(value >> (8 * i));
  }
  VtableRequest named_request() const { return {Name, 32, {}, 0}; }
  VtableRequest methods_request() const {
    return {"This name is intentionally not the runtime spelling", 32, {{8, code + 0x110}, {16, code + 0x120}}, 0};
  }
  VtableRequest root_request() const { return {{}, 32, {}, table}; }
  discovery::ReadWindow query(std::uint32_t at, std::uint32_t maximum) override {
    require(at < bytes.size() && maximum <= 32768 && maximum <= bytes.size() - at, "RTTI query exceeded the image/chunk bounds");
    if (oversized)
      return {maximum + 1, true};
    auto count = std::min(maximum, window);
    if (at < hole_begin)
      count = std::min(count, hole_begin - at);
    if (at >= hole_begin && at < hole_end)
      return {std::min(count, hole_end - at), false};
    return {count, true};
  }
  bool read(std::uint32_t at, void* output, std::size_t size) override {
    require(size && size <= 32768 && std::uint64_t(at) + size <= bytes.size(), "RTTI read escaped the image or exceeded a chunk");
    attempted += size;
    const auto count = ++reads[at];
    if (at == fail_at)
      return false;
    if (count == 2 && at == change_at)
      bytes[at] ^= 1;
    require(!(at < hole_end && std::uint64_t(at) + size > hole_begin), "RTTI reader bypassed an unreadable image window");
    std::memcpy(output, bytes.data() + at, size);
    return true;
  }
  RttiVtables run(const std::vector<VtableRequest>& requests) {
    auto result = resolve_rtti_vtables(*this, image, base, requests);
    require(result.read_bytes == attempted && result.scanned_bytes <= 128ull * 1024 * 1024 && result.read_calls <= 131072,
            "RTTI counters exceeded their bounds or lost attempted reads");
    if (!result.valid)
      require(result.vtables.empty() && !result.error.empty(), "RTTI refusal published partial addresses or omitted its reason");
    return result;
  }
};
void valid_relocated() {
  for (const auto shift : {0u, 0x20000u}) {
    for (const auto window : {17u, 4096u, 32768u}) {
      for (unsigned mode = 0; mode < 3; ++mode) {
        Fixture f(shift, shift ? 0x178000000 : 0x140000000);
        f.window = window;
        auto request = mode == 0 ? f.named_request() : mode == 1 ? f.methods_request() : f.root_request();
        const auto result = f.run({request});
        require(result.valid && result.error.empty() && result.vtables == std::vector<std::uint32_t>{f.table},
                "Moved primary RTTI/vtable failed bounded resolution");
      }
    }
  }
  Fixture no_name;
  no_name.hole_begin = no_name.type + 16;
  no_name.hole_end = no_name.type + 64;
  const auto result = no_name.run({no_name.methods_request()});
  require(result.valid, "Method-anchored resolution attempted to read an obfuscated type name");
  Fixture combined;
  auto root_and_methods = combined.methods_request();
  root_and_methods.resolved_vtable_rva = combined.table;
  require(combined.run({root_and_methods}).valid, "Constructor root and verified methods were not accepted together");
  Fixture ordered;
  constexpr std::uint32_t second_col = 0x5100, second_type = 0x16100, second_hierarchy = 0x5300, second_table = 0x7008;
  std::copy_n(ordered.bytes.begin() + ordered.col, 24, ordered.bytes.begin() + second_col);
  ordered.put(second_col + 12, second_type);
  ordered.put(second_col + 16, second_hierarchy);
  ordered.put(second_col + 20, second_col);
  ordered.put(second_type, 0x700001234000ull, 8);
  ordered.put(second_hierarchy + 8, 1);
  ordered.put(second_hierarchy + 12, second_hierarchy + 32);
  ordered.put(second_hierarchy + 32, second_hierarchy + 64);
  ordered.put(second_hierarchy + 64, second_type);
  ordered.put(second_hierarchy + 64 + 12, UINT32_MAX);
  ordered.put(second_hierarchy + 64 + 20, 64);
  ordered.put(second_hierarchy + 64 + 24, second_hierarchy);
  ordered.put(second_table - 8, ordered.base + second_col, 8);
  for (unsigned i = 0; i < 4; ++i)
    ordered.put(second_table + i * 8, ordered.base + ordered.code + 0x300 + i * 16, 8);
  VtableRequest second{{}, 32, {{8, ordered.code + 0x310}}, 0};
  const auto result_order = ordered.run({second, ordered.named_request()});
  require(result_order.valid && result_order.vtables == std::vector<std::uint32_t>{second_table, ordered.table},
          "Mixed method/name results followed scan order rather than request order");
}
void malformed() {
  for (unsigned mode = 0; mode < 3; ++mode) {
    for (unsigned scenario = 0; scenario < 21; ++scenario) {
      Fixture f;
      auto request = mode == 0 ? f.named_request() : mode == 1 ? f.methods_request() : f.root_request();
      switch (scenario) {
        case 0:
          f.put(f.col, 0);
          break;
        case 1:
          f.put(f.col + 4, 8);
          break;
        case 2:
          f.put(f.col + 8, 4);
          break;
        case 3:
          f.put(f.col + 20, f.col + 4);
          break;
        case 4:
          f.put(f.col + 12, f.code);
          break;
        case 5:
          f.put(f.col + 16, f.type);
          break;
        case 6:
          f.put(f.col + 12, 0x1f000);
          break;
        case 7:
          f.put(f.table - 8, f.base + f.code, 8);
          break;
        case 8:
          f.put(f.table, f.base + f.type, 8);
          break;
        case 9:
          f.put(f.table + 24, f.base + f.image.image_size, 8);
          break;
        case 10:
          f.image.sections[1].flags |= 0x80000000;
          break;
        case 11:
          f.image.sections[1].flags |= 0x20000000;
          break;
        case 12:
          f.image.sections[1].flags |= 0x02000000;
          break;
        case 13:
          f.image.sections[0].flags |= 0x80000000;
          break;
        case 14:
          request.extent = 4096;
          break;
        case 15:
          f.put(f.hierarchy, 1);
          break;
        case 16:
          f.put(f.hierarchy + 8, 65);
          break;
        case 17:
          f.put(f.hierarchy + 12, f.type);
          break;
        case 18:
          f.put(f.hierarchy + 32, f.type);
          break;
        case 19:
          f.put(f.hierarchy + 64, f.type + 8);
          break;
        case 20:
          f.put(f.hierarchy + 64 + 4, 1);
          break;
      }
      require(!f.run({request}).valid, "Invalid COL/section/endpoint was accepted");
    }
  }
  Fixture mismatch;
  auto request = mismatch.methods_request();
  request.methods[1].method_rva += 16;
  require(!mismatch.run({request}).valid, "A partial method match admitted the wrong vtable");
  Fixture unknown;
  request = unknown.named_request();
  request.type_name += 'X';
  require(!unknown.run({request}).valid, "A non-exact type name was accepted");
  Fixture unterminated;
  unterminated.put(unterminated.type + 16 + std::strlen(Fixture::Name), 'X', 1);
  require(!unterminated.run({unterminated.named_request()}).valid, "An unterminated type-name prefix was accepted");
}
void ambiguous_missing_and_changed() {
  for (unsigned mode = 0; mode < 3; ++mode) {
    Fixture f;
    const auto other = f.table + 0x100;
    for (unsigned i = 0; i < 40; ++i)
      f.bytes[other - 8 + i] = f.bytes[f.table - 8 + i];
    auto request = mode == 0 ? f.named_request() : mode == 1 ? f.methods_request() : f.root_request();
    const auto result = f.run({request});
    require(mode == 2 ? result.valid && result.vtables[0] == f.table : !result.valid && result.error == "ambiguous_primary_vtable",
            "Ambiguous discovery or an explicit constructor-derived root was mishandled");
  }
  Fixture missing;
  auto second = missing.named_request();
  second.type_name = ".?AVMissingFixture@@";
  require(!missing.run({missing.methods_request(), second}).valid, "Missing secondary request published a partial result");
  for (unsigned field = 0; field < 10; ++field) {
    Fixture f;
    const std::uint32_t addresses[]{f.col,        f.type,      f.hierarchy,  f.table - 8,      f.table,
                                    f.table + 24, f.table + 8, f.table + 16, f.hierarchy + 32, f.hierarchy + 64};
    f.change_at = addresses[field];
    const auto result = f.run({f.methods_request()});
    require(!result.valid && (result.error == "rtti_identity_changed" || result.error == "vtable_method_changed" ||
                              result.error == "rtti_hierarchy_changed"),
            "Accepted RTTI/type/hierarchy/slot changed without invalidating the result");
  }
  Fixture changed_name;
  changed_name.change_at = changed_name.type;
  require(!changed_name.run({changed_name.named_request()}).valid, "Changed named type identity escaped the final reread");
}
void bounds() {
  for (unsigned scenario = 0; scenario < 13; ++scenario) {
    Fixture f;
    auto request = f.methods_request();
    switch (scenario) {
      case 0:
        f.image.valid_image = false;
        break;
      case 1:
        f.base = UINT64_MAX - 0xff;
        break;
      case 2:
        f.image.sections[1].rva = f.code;
        break;
      case 3:
        f.image.sections[1].size = UINT32_MAX;
        break;
      case 4:
        request = {};
        break;
      case 5:
        request.extent = 4097;
        break;
      case 6:
        request.methods[0].offset = 32;
        break;
      case 7:
        request.methods[0].offset = 3;
        break;
      case 8:
        request.methods[0].method_rva = f.type;
        break;
      case 9:
        request.methods.push_back(request.methods[0]);
        break;
      case 10:
        request.resolved_vtable_rva = 1;
        break;
      case 11:
        request.resolved_vtable_rva = f.type;
        break;
      case 12:
        ++f.image.section_count;
        break;
    }
    require(!f.run({request}).valid && f.attempted == 0, "Malformed request/image caused target reads");
  }
  for (unsigned failure = 0; failure < 4; ++failure) {
    Fixture f;
    if (failure == 0) {
      f.hole_begin = 0x14000;
      f.hole_end = 0x14001;
    }
    if (failure == 1)
      f.fail_at = f.type;
    if (failure == 2)
      f.oversized = true;
    if (failure == 3)
      f.window = 0;
    require(!f.run({f.methods_request()}).valid, "Incomplete/unreadable scan returned a vtable");
  }
  Fixture limit;
  limit.image.image_size = 0x6000000;
  limit.image.section_count = 2;
  limit.image.sections = {{".text", 0x1000, 0x1000, 0x60000020}, {".rdata", 0x4000, 64 * 1024 * 1024 + 1, 0x40000040}};
  const auto capped = limit.run({limit.named_request()});
  require(!capped.valid && capped.error == "static_data_scan_limit" && limit.attempted == 0, "Static data cap was not enforced");
  Fixture fragmented;
  fragmented.window = 1;
  const auto reads = fragmented.run({fragmented.named_request()});
  require(!reads.valid && reads.error == "read_call_limit" && reads.read_calls == 131072, "Read-call cap was not enforced");
  Fixture col_cap;
  for (unsigned i = 0; i < 129; ++i) {
    const auto at = 0x6000 + i * 32;
    std::copy_n(col_cap.bytes.begin() + col_cap.col, 24, col_cap.bytes.begin() + at);
    col_cap.put(at + 20, at);
  }
  require(col_cap.run({col_cap.named_request()}).error == "rtti_candidate_limit", "Matched COL count was not bounded");
  Fixture pointer_cap;
  for (unsigned i = 0; i < 5000; ++i)
    pointer_cap.put(0x4000 + i * 8, pointer_cap.base + pointer_cap.code + 0x110, 8);
  require(pointer_cap.run({pointer_cap.methods_request()}).error == "vtable_candidate_limit", "Method pointer candidates were not bounded");
}
}  // namespace
int main() {
  try {
    valid_relocated();
    malformed();
    ambiguous_missing_and_changed();
    bounds();
    std::printf("RTTI vtable resolver: PASS %u checks; synthetic images only.\n", checks);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL RTTI vtable resolver after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
