#include "view_material_inventory.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
namespace taxi_camera::discovery {
namespace {
struct Observation {
  std::uint64_t address = 0;
  std::uint32_t size = 0;
  std::array<std::uint8_t, 16> value{};
};
class Reader {
 public:
  Reader(engine_camera::MemoryReader& source, ViewMaterialInventory& result) : source_(source), result_(result) {}
  bool fail(const char* error) {
    result_.error = error;
    return false;
  }
  bool field(std::uint64_t base, std::uint32_t offset, void* out, std::uint32_t bytes, bool control = false, bool remember = true) {
    if (!base || (!control && base % 8) || bytes == 0 || bytes > 16 || offset > UINT64_MAX - base || bytes > UINT64_MAX - base - offset)
      return fail("field_bounds");
    if (remember && count_ == observations_.size())
      return fail("observation_limit");
    if (!read(base + offset, out, bytes))
      return false;
    if (remember) {
      auto& item = observations_[count_++];
      item.address = base + offset;
      item.size = bytes;
      std::memcpy(item.value.data(), out, bytes);
    }
    return true;
  }
  bool word(std::uint64_t base, std::uint32_t offset, std::uint64_t& out, bool control = false) {
    return field(base, offset, &out, 8, control);
  }
  bool handle(std::uint64_t base,
              std::uint32_t offset,
              std::uint64_t& payload,
              std::uint32_t& generation,
              std::uint32_t& current,
              bool& stale) {
    std::array<std::uint8_t, 16> value{};
    payload = 0;
    stale = false;
    if (!field(base, offset, value.data(), 16))
      return false;
    std::uint64_t control = 0;
    std::memcpy(&control, value.data(), 8);
    std::memcpy(&generation, value.data() + 8, 4);
    if (!control)
      return true;
    if (!field(control, 28, &current, 4, true))
      return false;
    if (generation != current) {
      stale = true;
      return true;
    }
    return word(control, 0, payload, true);
  }
  bool recheck() {
    std::array<std::uint8_t, 16> value{};
    for (std::size_t i = 0; i < count_; ++i) {
      const auto& item = observations_[i];
      if (!read(item.address, value.data(), item.size))
        return false;
      if (!std::equal(value.begin(), value.begin() + item.size, item.value.begin()))
        return fail("identity_or_descriptor_changed");
    }
    return true;
  }

 private:
  bool read(std::uint64_t address, void* out, std::uint32_t bytes) {
    if (result_.read_bytes > 32768 - bytes)
      return fail("read_budget");
    result_.read_bytes += bytes;
    if (source_.read(address, out, bytes))
      return true;
    ++result_.read_failures;
    return fail("exact_read_failed");
  }
  engine_camera::MemoryReader& source_;
  ViewMaterialInventory& result_;
  std::array<Observation, 512> observations_{};
  std::size_t count_ = 0;
};
bool bitmap(Reader& source, std::uint64_t material, ViewBitmapMetadata& out, std::uint64_t& resource) {
  std::uint64_t object = 0;
  resource = 0;
  if (!source.handle(material, out.slot == 0 ? 520 : 664, object, out.handle_generation, out.current_generation, out.stale))
    return false;
  if (!object)
    return true;
  out.present = true;
  if (!source.field(object, 40, &out.bitmap_width, 4) || !source.field(object, 44, &out.bitmap_height, 4))
    return false;
  std::uint64_t record = 0, wrapper = 0;
  if (!source.word(object, 88, record))
    return false;
  if (!record || record == UINT64_MAX)
    return true;
  if (!source.word(record, 16, wrapper))
    return false;
  if (!wrapper)
    return true;
  std::array<std::uint8_t, 56> descriptor{};
  for (std::uint32_t offset = 0; offset < descriptor.size(); offset += 8)
    if (!source.field(wrapper, 96 + offset, descriptor.data() + offset, 8))
      return false;
  std::memcpy(&out.dimension, descriptor.data(), 4);
  std::memcpy(&out.width, descriptor.data() + 16, 8);
  std::memcpy(&out.height, descriptor.data() + 24, 4);
  std::memcpy(&out.layers, descriptor.data() + 28, 2);
  std::memcpy(&out.mips, descriptor.data() + 30, 2);
  std::memcpy(&out.format, descriptor.data() + 32, 4);
  std::memcpy(&out.samples, descriptor.data() + 36, 4);
  std::memcpy(&out.flags, descriptor.data() + 48, 4);
  if (!source.word(wrapper, 168, resource))
    return false;
  if (resource && resource % 8)
    return source.fail("resource_pointer_alignment");
  out.resource_present = resource != 0;
  return true;
}
}  // namespace
ViewMaterialInventory inspect_view_materials(engine_camera::MemoryReader& memory, std::uint64_t renderer) {
  ViewMaterialInventory result;
  Reader source(memory, result);
  std::uint64_t array = 0;
  std::array<std::uint64_t, 8> views{};
  std::array<std::uint64_t, 16> resources{};
  unsigned resource_count = 0;
  if (!source.word(renderer, 2752, array))
    return result;
  if (!array) {
    result.error = "view_pool_absent";
    return result;
  }
  for (unsigned i = 0; i < views.size(); ++i) {
    if (!source.word(array, i * 8, views[i]))
      return result;
    if (!views[i] || std::find(views.begin(), views.begin() + i, views[i]) != views.begin() + i) {
      result.error = "invalid_or_duplicate_view";
      return result;
    }
    auto& view = result.views[i];
    view.index = i;
    for (unsigned pair = 0; pair < 3; ++pair)
      if (!source.field(views[i], 16 + pair * 8, view.dimensions.data() + pair * 2, 8))
        return result;
    if (!source.field(views[i], 48, view.flags_before.data(), 16, false, false))
      return result;
    std::uint64_t material = 0;
    std::uint32_t generation = 0, current = 0;
    bool stale = false;
    if (!source.handle(views[i], 144, material, generation, current, stale))
      return result;
    view.material_present = material != 0;
    view.bitmaps[0].slot = 0;
    view.bitmaps[1].slot = 9;
    if (!material)
      continue;
    for (auto& item : view.bitmaps) {
      std::uint64_t resource = 0;
      if (!bitmap(source, material, item, resource))
        return result;
      if (resource) {
        auto found = std::find(resources.begin(), resources.begin() + resource_count, resource);
        if (found == resources.begin() + resource_count)
          resources[resource_count++] = resource;
        item.resource_ordinal = static_cast<std::uint32_t>(found - resources.begin()) + 1;
      }
    }
  }
  if (!source.recheck())
    return result;
  for (unsigned i = 0; i < views.size(); ++i) {
    auto& view = result.views[i];
    if (!source.field(views[i], 48, view.flags_after.data(), 16, false, false))
      return result;
    view.flags_stable = view.flags_before == view.flags_after;
  }
  result.complete = true;
  result.error = "";
  return result;
}
}  // namespace taxi_camera::discovery
