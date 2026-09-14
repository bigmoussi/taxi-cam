#include "../../tools/diagnostics/command_list_inventory.hpp"

#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
using namespace taxi_camera::discovery;
constexpr std::uint64_t Base = 0x140000000;
constexpr std::uint64_t Renderer = 0x240000000;
constexpr std::uint32_t Global = 173790384;
constexpr std::uint32_t Vtable = 134482536;
constexpr std::uint32_t Offset = 284576;
std::size_t checks = 0;

void require(bool condition, const char* reason) {
  ++checks;
  if (!condition)
    throw std::runtime_error(reason);
}

void store(void* destination, std::uint64_t value, std::size_t size) {
  for (std::size_t i = 0; i < size; ++i)
    static_cast<std::uint8_t*>(destination)[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

struct Image final : ImageReader {
  std::uint64_t global = Renderer;
  bool mutate = false;
  bool readable = true;
  std::size_t short_window = 0;
  unsigned fail = 0;
  unsigned queries = 0;
  unsigned reads = 0;
  ReadWindow query(std::uint32_t rva, std::uint32_t maximum) override {
    require(rva == Global && maximum == 8, "Unexpected image query");
    ++queries;
    return {static_cast<std::uint32_t>(short_window ? short_window : 8), readable};
  }
  bool read(std::uint32_t rva, void* destination, std::size_t size) override {
    require(rva == Global && size == 8, "Unexpected image read");
    ++reads;
    if (reads == fail)
      return false;
    store(destination, global + ((mutate && reads == 2) ? 8 : 0), size);
    return true;
  }
};

struct Objects final : CommandListObjectReader {
  std::uint64_t vptr = Base + Vtable;
  std::uint32_t type = 3;
  bool mutate_vptr = false;
  bool mutate_type = false;
  unsigned fail = 0;
  std::vector<std::pair<std::uint64_t, std::size_t>> reads;
  bool read(std::uint64_t address, void* destination, std::size_t size) override {
    reads.emplace_back(address, size);
    require((address == Renderer && size == 8) || (address == Renderer + Offset && size == 4), "Unexpected object field");
    if (reads.size() == fail)
      return false;
    const auto value =
        size == 8 ? vptr + ((mutate_vptr && reads.size() == 4) ? 8 : 0) : type + ((mutate_type && reads.size() == 3) ? 1 : 0);
    store(destination, value, size);
    return true;
  }
};

struct Fixture {
  Image reader;
  Objects objects;
  Inventory image;
  FunctionInventory proof;
  Fixture() {
    image.valid_image = true;
    image.machine = 0x8664;
    image.timestamp = 1787653788;
    image.image_size = 235963904;
    image.section_count = 14;
    image.sections = {{".text", 64597824, 680, 0x60000020}, {".rdata", Vtable, 8, 0x40000040}, {".data", Global, 8, 0xc0000040}};
    proof.valid_targets = true;
    proof.code_bytes = proof.decoded_bytes = 680;
    FunctionContext f;
    f.requested_rva = f.function_begin_rva = 64597824;
    f.function_end_rva = 64598504;
    f.bytes_read = f.decoded_bytes = 680;
    f.requested_boundary_found = true;
    f.instructions = {{64597849, "\tmovq\t%rcx, %rdi", 0},       {64598075, "\tmovq\t(%rdi), %rcx", 0},
                      {64598089, "\tmovq\t(%rcx), %rdx", 0},     {64598099, "\tmovq\t96(%rdx), %rax", 0},
                      {64598103, "\tmovq\t%r8, 48(%rsp)", 0},    {64598115, "\tmovq\t%rdx, 40(%rsp)", 0},
                      {64598120, "\tmovq\t%rbx, 32(%rsp)", 0},   {64598125, "\tmovq\t16(%rdi), %r9", 0},
                      {64598129, "\tmovl\t208(%rdi), %r8d", 0},  {64598136, "\txorl\t%edx, %edx", 0},
                      {64598138, "\tcallq\t*63307464(%rip)", 0}, {64598461, "\tmovq\t24(%rdi), %rax", 0},
                      {64598465, "\tmovq\t%rax, (%r15)", 0},     {64598503, "\tretq", 0}};
    proof.functions.push_back(f);
  }
  CommandListInventory run(std::uint64_t base = Base) {
    auto result = inspect_command_list_type(reader, objects, image, base, proof);
    std::size_t bytes = 0;
    for (const auto& read : objects.reads)
      bytes += read.second;
    require(result.object_bytes == bytes && bytes <= 24 && result.image_bytes == reader.queries * 8 && result.image_bytes <= 16,
            "Read accounting/cap failed");
    require(!result.available || result.valid, "Partial result exposed available type");
    return result;
  }
};
}  // namespace

int main() {
  try {
    for (std::uint32_t type = 0; type < 4; ++type) {
      Fixture f;
      f.objects.type = type;
      const auto result = f.run();
      require(result.valid && result.available && result.type == type && result.stage == "complete" && result.error.empty() &&
                  result.read_failures == 0 && result.image_bytes == 16 && result.object_bytes == 24 && result.vtable_rva == Vtable,
              "Valid type not preserved");
      require(f.objects.reads ==
                  std::vector<std::pair<std::uint64_t, std::size_t>>{
                      {Renderer, 8}, {Renderer + Offset, 4}, {Renderer + Offset, 4}, {Renderer, 8}},
              "Object trace changed");
    }
    {
      Fixture f;
      f.reader.global = 0;
      const auto r = f.run();
      require(r.valid && !r.available && r.object_bytes == 0, "Null renderer followed");
    }
    for (auto bad : {std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max() - 10}) {
      Fixture f;
      require(!f.run(bad).valid && f.reader.queries == 0, "Bad image base accepted");
    }
    for (auto bad : {Renderer + 1, std::numeric_limits<std::uint64_t>::max() - 7}) {
      Fixture f;
      f.reader.global = bad;
      require(!f.run().valid && f.objects.reads.empty(), "Bad renderer followed");
    }
    for (auto bad : {std::uint64_t{0}, Base - 8, Base + Vtable + 8}) {
      Fixture f;
      f.objects.vptr = bad;
      const auto r = f.run();
      require(!r.valid && r.object_bytes == 8, "Unknown renderer accepted");
    }
    for (auto bad : {4u, 6u, 0xffffffffu}) {
      Fixture f;
      f.objects.type = bad;
      const auto r = f.run();
      require(!r.valid && !r.available && r.stage == "command_list_type", "Invalid type accepted");
    }
    for (unsigned fail = 1; fail <= 4; ++fail) {
      Fixture f;
      f.objects.fail = fail;
      const auto r = f.run();
      require(!r.valid && r.read_failures == 1 && f.objects.reads.size() == fail, "Object failure continued");
    }
    for (unsigned fail = 1; fail <= 2; ++fail) {
      Fixture f;
      f.reader.fail = fail;
      const auto r = f.run();
      require(!r.valid && r.read_failures == 1 && f.reader.reads == fail, "Image failure ignored");
    }
    {
      Fixture f;
      f.reader.readable = false;
      const auto r = f.run();
      require(!r.valid && r.read_failures == 1 && f.reader.reads == 0, "Unreadable query ignored");
    }
    {
      Fixture f;
      f.reader.short_window = 7;
      require(!f.run().valid && f.reader.reads == 0, "Short query ignored");
    }
    {
      Fixture f;
      f.reader.mutate = true;
      const auto r = f.run();
      require(!r.valid && !r.available && r.stage == "global_recheck", "Changed renderer accepted");
    }
    {
      Fixture f;
      f.objects.mutate_vptr = true;
      const auto r = f.run();
      require(!r.valid && !r.available && r.stage == "vptr_recheck", "Changed vptr accepted");
    }
    {
      Fixture f;
      f.objects.mutate_type = true;
      const auto r = f.run();
      require(!r.valid && !r.available && r.stage == "type_recheck", "Changed type accepted");
    }
    for (std::size_t index = 0; index < 14; ++index) {
      Fixture f;
      f.proof.functions[0].instructions[index].text += " ";
      require(!f.run().valid && f.reader.queries == 0, "Changed signature accepted");
    }
    for (unsigned test = 0; test < 17; ++test) {
      Fixture f;
      switch (test) {
        case 0:
          f.image.valid_image = false;
          break;
        case 1:
          f.image.timestamp++;
          break;
        case 2:
          f.image.image_size--;
          break;
        case 3:
          f.image.section_count--;
          break;
        case 4:
          f.image.machine = 0x14c;
          break;
        case 5:
          f.proof.valid_targets = false;
          break;
        case 6:
          f.proof.partial = true;
          break;
        case 7:
          f.proof.error = "failure";
          break;
        case 8:
          f.proof.read_failures = 1;
          break;
        case 9:
          f.proof.functions[0].truncated = true;
          break;
        case 10:
          f.proof.functions[0].decode_failed = true;
          break;
        case 11:
          f.proof.functions[0].requested_boundary_found = false;
          break;
        case 12:
          f.proof.functions[0].function_end_rva--;
          break;
        case 13:
          f.proof.functions[0].bytes_read--;
          break;
        case 14:
          f.proof.functions[0].decoded_bytes--;
          break;
        case 15:
          f.proof.code_bytes--;
          break;
        case 16:
          f.proof.functions.clear();
          break;
      }
      require(!f.run().valid && f.reader.queries == 0, "Incomplete build/code proof accessed object");
    }
    for (std::size_t index = 0; index < 3; ++index) {
      for (auto bit : {0x40000000u, 0x20000000u, 0x02000000u}) {
        Fixture f;
        f.image.sections[index].flags ^= bit;
        require(!f.run().valid && f.reader.queries == 0, "Bad section permissions accepted");
      }
      Fixture f;
      f.image.sections[index].size--;
      require(!f.run().valid && f.reader.queries == 0, "Section boundary overread");
    }
    std::printf("PASS: %zu fixed command-list type proof, scope, mutation and failure checks.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
