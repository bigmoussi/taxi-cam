#include "view_material_inventory.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
namespace d = taxi_camera::discovery;
struct Memory final : taxi_camera::engine_camera::MemoryReader {
  std::vector<unsigned char> bytes = std::vector<unsigned char>(0x40000);
  std::uint64_t refused = 0, changed = 0;
  unsigned change_reads = 0, reads = 0;
  template <class T>
  void put(std::uint64_t address, T value) {
    std::memcpy(bytes.data() + address, &value, sizeof(value));
  }
  bool read(std::uint64_t address, void* out, std::size_t size) override {
    ++reads;
    if (address == refused || address > bytes.size() || size > bytes.size() - address)
      return false;
    std::memcpy(out, bytes.data() + address, size);
    if (address == changed && ++change_reads == 2)
      static_cast<unsigned char*>(out)[0] ^= 1;
    return true;
  }
  void handle(std::uint64_t address, std::uint64_t control, std::uint64_t payload) {
    put(address, control);
    put(address + 8, std::uint32_t(13));
    put(control + 28, std::uint32_t(13));
    put(control, payload);
  }
  Memory() {
    put(0x1000 + 2752, std::uint64_t(0x3000));
    for (unsigned i = 0; i < 8; ++i) {
      const std::uint64_t view = 0x5000 + i * 0x400, material = 0xa000 + i * 0x800;
      put(0x3000 + i * 8, view);
      for (unsigned pair = 0; pair < 3; ++pair) {
        put(view + 16 + pair * 8, std::uint32_t(768));
        put(view + 20 + pair * 8, std::uint32_t(504));
      }
      put(view + 48, std::uint64_t(0x2000000000001));
      put(view + 56, std::uint64_t(0x80));
      handle(view + 144, 0x9001 + i * 0x40, material);
      for (unsigned slot = 0; slot < 2; ++slot) {
        const std::uint64_t object = 0x10000 + i * 0x1800 + slot * 0x900;
        handle(material + (slot ? 664 : 520), object + 0x101, object);
        put(object + 40, std::uint32_t(slot ? 384 : 768));
        put(object + 44, std::uint32_t(slot ? 252 : 504));
        put(object + 88, object + 0x200);
        put(object + 0x200 + 16, object + 0x300);
        const auto descriptor = object + 0x300 + 96;
        put(descriptor, std::uint32_t(3));
        put(descriptor + 16, std::uint64_t(slot ? 384 : 768));
        put(descriptor + 24, std::uint32_t(slot ? 252 : 504));
        put(descriptor + 28, std::uint16_t(1));
        put(descriptor + 30, std::uint16_t(1));
        put(descriptor + 32, std::uint32_t(slot ? 10 : 26));
        put(descriptor + 36, std::uint32_t(1));
        put(descriptor + 48, std::uint32_t(1));
        put(object + 0x300 + 168, object + 0x600);
      }
    }
  }
};
int main() {
  unsigned checks = 0;
  const auto check = [&](bool valid) {
    if (!valid) {
      std::fprintf(stderr, "view material check%u failed\n", checks);
      std::exit(1);
    }
    ++checks;
  };
  Memory memory;
  auto result = d::inspect_view_materials(memory, 0x1000);
  check(result.complete && result.read_failures == 0 && result.read_bytes <= 32768);
  for (unsigned i = 0; i < 8; ++i) {
    const auto& view = result.views[i];
    check(view.index == i && view.material_present && view.flags_stable);
    for (unsigned s = 0; s < 2; ++s) {
      const auto& b = view.bitmaps[s];
      check(b.slot == (s ? 9u : 0u) && b.present && b.resource_present && !b.stale);
      check(b.handle_generation == 13 && b.current_generation == 13 && b.resource_ordinal == i * 2 + s + 1);
      check(b.width == (s ? 384u : 768u) && b.bitmap_width == b.width && b.height == (s ? 252u : 504u) && b.bitmap_height == b.height);
      check(b.dimension == 3 && b.layers == 1 && b.mips == 1 && b.format == (s ? 10u : 26u) && b.samples == 1 && b.flags == 1);
    }
  }
  for (const auto bad : {std::uint64_t(0), std::uint64_t(1), UINT64_MAX - 7}) {
    Memory f;
    const auto r = d::inspect_view_materials(f, bad);
    check(!r.complete);
    check(f.reads == 0);
  }
  {
    Memory f;
    f.put(0x3008, std::uint64_t(0x5000));
    check(!d::inspect_view_materials(f, 0x1000).complete);
  }
  {
    Memory f;
    f.put(0x5000 + 144, std::uint64_t(0));
    const auto r = d::inspect_view_materials(f, 0x1000);
    check(r.complete && !r.views[0].material_present);
  }
  {
    Memory f;
    f.put(0xa000 + 664, std::uint64_t(0));
    const auto r = d::inspect_view_materials(f, 0x1000);
    check(r.complete && !r.views[0].bitmaps[1].present);
  }
  {
    Memory f;
    f.put(0x10900 + 0x101 + 28, std::uint32_t(14));
    f.put(0x10900 + 0x101, UINT64_MAX);
    const auto r = d::inspect_view_materials(f, 0x1000);
    check(r.complete && r.views[0].bitmaps[1].stale && !r.views[0].bitmaps[1].present);
  }
  {
    Memory f;
    f.put(0x10000 + 88, UINT64_MAX);
    const auto r = d::inspect_view_materials(f, 0x1000);
    check(r.complete && !r.views[0].bitmaps[0].resource_present);
  }
  {
    Memory f;
    f.put(0x10900 + 0x300 + 168, std::uint64_t(0x10600));
    const auto r = d::inspect_view_materials(f, 0x1000);
    check(r.complete && r.views[0].bitmaps[0].resource_ordinal == r.views[0].bitmaps[1].resource_ordinal);
  }
  for (const auto address :
       {std::uint64_t(0x3000), std::uint64_t(0x5000 + 144), std::uint64_t(0x10000 + 0x300 + 96), std::uint64_t(0x10000 + 0x300 + 168)}) {
    Memory f;
    f.changed = address;
    check(!d::inspect_view_materials(f, 0x1000).complete);
  }
  {
    Memory f;
    f.changed = 0x5000 + 48;
    const auto r = d::inspect_view_materials(f, 0x1000);
    check(r.complete && !r.views[0].flags_stable && r.views[0].flags_before != r.views[0].flags_after);
  }
  {
    Memory f;
    f.refused = 0x10000 + 0x300 + 96;
    const auto r = d::inspect_view_materials(f, 0x1000);
    check(!r.complete && r.read_failures == 1);
  }
  std::printf("{\"passed\":true,\"checks\":%u,\"full_read_bytes\":%u}\n", checks, result.read_bytes);
}
