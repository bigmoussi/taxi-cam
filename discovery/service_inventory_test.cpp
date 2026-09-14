#include "service_inventory.hpp"

#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace {
using namespace taxi_camera::discovery;
constexpr std::uint64_t Base = 0x140000000;
constexpr std::uint64_t Object = 0x220000008;
constexpr std::uint32_t Vtable = 0x2100;
constexpr std::uint32_t Slot = Vtable + kServiceMethodOffset;
constexpr std::uint32_t Method = 0x1100;
constexpr std::uint32_t CleanupSlot = Vtable + kServiceCleanupMethodOffset;
constexpr std::uint32_t CleanupMethod = 0x1200;

void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct Reader final : ImageReader {
  std::map<std::uint32_t, std::uint64_t> words = {{kServiceGlobalRva, Object}, {Slot, Base + Method}, {CleanupSlot, Base + CleanupMethod}};
  std::vector<std::pair<std::uint32_t, std::uint32_t>> reads;
  std::uint32_t fail_rva = 0;
  ReadWindow query(std::uint32_t, std::uint32_t maximum) override { return {maximum, true}; }
  bool read(std::uint32_t rva, void* destination, std::size_t size) override {
    reads.emplace_back(rva, static_cast<std::uint32_t>(size));
    if (rva == fail_rva || size != 8 || !words.contains(rva))
      return false;
    auto* bytes = static_cast<std::uint8_t*>(destination);
    for (std::uint32_t i = 0; i < 8; ++i)
      bytes[i] = static_cast<std::uint8_t>(words.at(rva) >> (i * 8));
    return true;
  }
};

struct ObjectReader final : ObjectVptrReader {
  std::uint64_t vptr = Base + Vtable;
  bool fail_read = false;
  std::vector<std::uint64_t> reads;
  bool read_vptr(std::uint64_t object_address, std::uint64_t& value) override {
    reads.push_back(object_address);
    if (fail_read)
      return false;
    value = vptr;
    return true;
  }
};

struct Fixture {
  Reader reader;
  ObjectReader objects;
  Inventory image;
  Fixture() {
    image.valid_image = true;
    image.machine = 0x8664;
    image.image_size = 200000000;
    image.sections = {
        {".text", 0x1000, 0x1000, 0x60000020}, {".rdata", 0x2000, 0x2000, 0x40000040}, {".data", kServiceGlobalRva - 16, 4096, 0xc0000040}};
  }
  ServiceInventory run(std::uint64_t base = Base) {
    reader.reads.clear();
    objects.reads.clear();
    auto result = inspect_service_metadata(reader, objects, image, base);
    require(objects.reads.size() <= 1 && reader.reads.size() <= 3, "Read count exceeded the fixed metadata chain");
    require(result.image_bytes == reader.reads.size() * 8 && result.object_bytes == objects.reads.size() * 8 &&
                result.image_bytes + result.object_bytes <= 32,
            "Attempted metadata byte accounting exceeded the fixed cap");
    for (const auto& read : reader.reads)
      require(read.second == 8 && (read.first == kServiceGlobalRva || read.first == Slot || read.first == CleanupSlot),
              "Reader followed a target or read an unapproved image field");
    return result;
  }
};
}  // namespace

int main() {
  try {
    Fixture test;
    auto result = test.run();
    require(
        result.valid && result.cached_present && result.stage == "complete" && result.error.empty() && result.read_failures == 0 &&
            result.image_bytes == 24 && result.object_bytes == 8 && result.vtable_rva == Vtable && result.method_slot_rva == Slot &&
            result.method_rva == Method && result.cleanup_slot_rva == CleanupSlot && result.cleanup_method_rva == CleanupMethod &&
            test.objects.reads == std::vector<std::uint64_t>{Object} &&
            test.reader.reads == std::vector<std::pair<std::uint32_t, std::uint32_t>>{{kServiceGlobalRva, 8}, {Slot, 8}, {CleanupSlot, 8}},
        "The valid fixed metadata chain was not resolved to RVAs");

    test.reader.words[kServiceGlobalRva] = 0;
    result = test.run();
    require(result.valid && !result.cached_present && result.stage == "unavailable" && result.error.empty() && result.image_bytes == 8 &&
                result.object_bytes == 0 && result.method_rva == 0 && result.cleanup_slot_rva == 0 && result.cleanup_method_rva == 0,
            "A null cached service did not stop before the object read");

    for (const auto invalid_base : {std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max() - 100}) {
      test = Fixture{};
      result = test.run(invalid_base);
      require(!result.valid && result.stage == "image_validation" && result.image_bytes == 0, "Null or overflowed image base was accepted");
    }
    test = Fixture{};
    test.image.valid_image = false;
    require(!test.run().valid && test.reader.reads.empty(), "An invalid image was read");
    test = Fixture{};
    test.image.machine = 0x14c;
    require(!test.run().valid && test.reader.reads.empty(), "A non-AMD64 image was read");
    test = Fixture{};
    test.image.image_size = 0;
    require(!test.run().valid && test.reader.reads.empty(), "An empty image was read");

    for (const auto flags : {0u, 0x40000040u, 0x80000040u, 0xe0000040u, 0xc2000040u}) {
      test = Fixture{};
      test.image.sections[2].flags = flags;
      result = test.run();
      require(!result.valid && result.stage == "cached_global" && result.image_bytes == 0,
              "Wrong cached-global section permissions were accepted");
    }
    test = Fixture{};
    test.image.sections.pop_back();
    require(!test.run().valid && test.reader.reads.empty(), "An undeclared cached-global slot was read");
    test = Fixture{};
    test.image.image_size = kServiceGlobalRva + 7;
    require(!test.run().valid && test.reader.reads.empty(), "A cached-global word beyond the image was read");
    test = Fixture{};
    test.image.sections[2].size = 23;
    require(!test.run().valid && test.reader.reads.empty(), "A cached-global word beyond its section was read");
    test = Fixture{};
    test.image.sections[2].size = std::numeric_limits<std::uint32_t>::max();
    require(!test.run().valid && test.reader.reads.empty(), "Overflowed section extent was accepted");

    test = Fixture{};
    test.reader.fail_rva = kServiceGlobalRva;
    result = test.run();
    require(!result.valid && result.stage == "cached_global" && result.read_failures == 1 && result.image_bytes == 8 &&
                result.object_bytes == 0,
            "A failed cached-global read proceeded to the object");
    test = Fixture{};
    test.reader.words[kServiceGlobalRva] = std::numeric_limits<std::uint64_t>::max() - 3;
    result = test.run();
    require(!result.valid && result.cached_present && result.stage == "object_vptr" && result.object_bytes == 0,
            "An overflowing object-word address was forwarded to the adapter");
    test = Fixture{};
    test.objects.fail_read = true;
    result = test.run();
    require(
        !result.valid && result.stage == "object_vptr" && result.read_failures == 1 && result.image_bytes == 8 && result.object_bytes == 8,
        "A failed object read proceeded to a method slot");

    for (const auto vptr : {std::uint64_t{0}, Base - 1, Base + 200000000, std::numeric_limits<std::uint64_t>::max(), Base + 0x1000,
                            Base + kServiceGlobalRva, Base + 0x3ff9}) {
      test = Fixture{};
      test.objects.vptr = vptr;
      result = test.run();
      require(!result.valid && result.stage == "vtable_validation" && result.image_bytes == 8 && result.vtable_rva == 0 &&
                  result.method_slot_rva == 0 && result.cleanup_slot_rva == 0,
              "An invalid, overflowing or non-static vtable was accepted");
    }
    for (const auto flags : {0u, 0xc0000040u, 0x60000040u, 0x42000040u}) {
      test = Fixture{};
      test.image.sections[1].flags = flags;
      result = test.run();
      require(!result.valid && result.stage == "vtable_validation" && result.image_bytes == 8,
              "Wrong vtable section permissions were accepted");
    }
    test = Fixture{};
    test.image.sections[1].size = Slot + 7 - test.image.sections[1].rva;
    require(!test.run().valid && test.reader.reads.size() == 1, "The method slot crossed the vtable section end");
    test = Fixture{};
    test.image.sections[1].size = CleanupSlot + 7 - test.image.sections[1].rva;
    require(!test.run().valid && test.reader.reads.size() == 1, "A truncated cleanup slot proceeded to method-pointer reads");
    test = Fixture{};
    test.image.sections[1].size = Slot + 8 - test.image.sections[1].rva;
    require(test.run().valid, "The exact complete vtable-prefix boundary was refused");
    test = Fixture{};
    test.image.sections[1].size = CleanupSlot - test.image.sections[1].rva;
    test.image.sections.push_back({".rdata_tail", CleanupSlot + 8, 0x1000, 0x40000040});
    require(!test.run().valid && test.reader.reads.size() == 1,
            "A missing cleanup-slot region was accepted because the later method slot was declared read-only");
    test = Fixture{};
    require(!test.run(Base + 0x100000).valid, "A stale loaded-image base was accepted");

    test = Fixture{};
    test.reader.fail_rva = Slot;
    result = test.run();
    require(!result.valid && result.stage == "method_slot" && result.read_failures == 1 && result.image_bytes == 16 &&
                result.object_bytes == 8 && result.vtable_rva == Vtable && result.method_slot_rva == Slot && result.method_rva == 0 &&
                result.cleanup_slot_rva == CleanupSlot && result.cleanup_method_rva == 0,
            "A failed method-slot read was accepted");
    for (const auto method : {std::uint64_t{0}, Base - 1, Base + 200000000, std::numeric_limits<std::uint64_t>::max(), Base + Vtable,
                              Base + kServiceGlobalRva}) {
      test = Fixture{};
      test.reader.words[Slot] = method;
      result = test.run();
      require(!result.valid && result.stage == "method_target" && result.read_failures == 0 && result.method_rva == 0 &&
                  result.image_bytes == 16 && result.cleanup_method_rva == 0,
              "A non-executable or foreign method target was accepted");
    }
    for (const auto flags : {0u, 0x40000020u, 0xe0000020u, 0x62000020u}) {
      test = Fixture{};
      test.image.sections[0].flags = flags;
      require(!test.run().valid, "Wrong method-code section permissions were accepted");
    }
    test = Fixture{};
    test.reader.fail_rva = CleanupSlot;
    result = test.run();
    require(!result.valid && result.stage == "cleanup_slot" && !result.error.empty() && result.read_failures == 1 &&
                result.image_bytes == 24 && result.object_bytes == 8 && result.vtable_rva == Vtable && result.method_slot_rva == Slot &&
                result.method_rva == Method && result.cleanup_slot_rva == CleanupSlot && result.cleanup_method_rva == 0,
            "A failed cleanup-slot read hid partial metadata or incorrectly completed the inventory");
    for (const auto method : {std::uint64_t{0}, Base - 1, Base + 200000000, std::numeric_limits<std::uint64_t>::max(), Base + Vtable,
                              Base + kServiceGlobalRva}) {
      test = Fixture{};
      test.reader.words[CleanupSlot] = method;
      result = test.run();
      require(!result.valid && result.stage == "cleanup_target" && !result.error.empty() && result.read_failures == 0 &&
                  result.image_bytes == 24 && result.method_rva == Method && result.cleanup_slot_rva == CleanupSlot &&
                  result.cleanup_method_rva == 0,
              "A non-executable or foreign cleanup target was accepted or lost the previously verified method");
    }
    for (const auto flags : {0u, 0x40000020u, 0xe0000020u, 0x62000020u}) {
      test = Fixture{};
      test.reader.words[CleanupSlot] = Base + 0x5100;
      test.image.sections.push_back({".cleanup", 0x5000, 0x1000, flags});
      result = test.run();
      require(!result.valid && result.stage == "cleanup_target" && result.method_rva == Method && result.cleanup_method_rva == 0,
              "Wrong cleanup-code section permissions were accepted");
    }
    test = Fixture{};
    test.reader.words[CleanupSlot] = Base + 0x1fff;
    result = test.run();
    require(result.valid && result.cleanup_method_rva == 0x1fff && result.image_bytes == 24,
            "An executable cleanup target at the exact last section byte was refused");
    test = Fixture{};
    test.reader.words[CleanupSlot] = Base + Method;
    require(test.run().valid, "Two fixed slots were incorrectly required to have distinct executable targets");
    std::puts("PASS: two fixed service method slots, partial-metadata refusals, null cache, RVA normalization, bounds and 32-byte cap.");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
