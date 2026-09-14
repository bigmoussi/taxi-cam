#include "observer_hook.hpp"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <stdexcept>

namespace {
using namespace taxi_camera::engine_hook;

unsigned fail_readonly = 0;
unsigned fail_writable = 0;
unsigned protection_calls = 0;
unsigned fail_exact_call = 0;
unsigned executable_protection_calls = 0;
void* replace_after_writable = nullptr;
void** mock_slot = nullptr;
unsigned original_calls = 0;
unsigned observer_calls = 0;

void original_target(void*) noexcept {
  ++original_calls;
}

void foreign_target(void*) noexcept {}

void observer(void*) noexcept {
  ++observer_calls;
}

// A real, initially read-only PE image slot. Only this validation executable's
// page is changed; production tests never open or alter another process.
alignas(4096) __attribute__((section(".taxiro"), used)) void* const image_slot = reinterpret_cast<void*>(&original_target);
alignas(4096) __attribute__((section(".taxirw"), used)) void* writable_image_slot = reinterpret_cast<void*>(&original_target);

__attribute__((section(".taxix"), aligned(4096), naked)) void executable_image_fixture() {
  __asm__("ret\n\t.byte 0,0,0,0,0,0,0");
}

void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

DWORD protection(void* address) {
  MEMORY_BASIC_INFORMATION region{};
  require(VirtualQuery(address, &region, sizeof(region)) == sizeof(region), "Cannot query mock table protection");
  return region.Protect;
}

struct MockTable {
  void** slot = static_cast<void**>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));

  MockTable() {
    require(slot != nullptr, "Cannot allocate mock table data");
    *slot = reinterpret_cast<void*>(&original_target);
    DWORD prior = 0;
    require(VirtualProtect(slot, 4096, PAGE_READONLY, &prior) != FALSE, "Cannot protect mock vtable");
    mock_slot = slot;
  }
  ~MockTable() { VirtualFree(slot, 0, MEM_RELEASE); }

  void reset_foreign_for_test() {
    DWORD prior = 0;
    require(VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prior) != FALSE, "Cannot reset foreign mock slot");
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), reinterpret_cast<void*>(&original_target));
    DWORD discarded = 0;
    require(VirtualProtect(slot, sizeof(void*), prior, &discarded) != FALSE, "Cannot restore reset mock page protection");
  }
};

void require_restore_failure(Result result, bool changed) {
  require(result.status == Status::protection_restore_failed && result.windows_error == ERROR_ACCESS_DENIED &&
              result.pointer_changed == changed && !result.protection_restored,
          "A failed protection restoration was not reported precisely");
}

void verify_removed(MockTable& table) {
  const auto result = remove();
  require(result.status == Status::removed && result.pointer_changed && result.protection_restored &&
              *table.slot == reinterpret_cast<void*>(&original_target) && protection(table.slot) == PAGE_READONLY,
          "Recovery did not allow removal with the original read-only protection");
}

void install_restore_failure() {
  MockTable table;
  fail_readonly = 2;
  require_restore_failure(install(table.slot, reinterpret_cast<void*>(&original_target), &observer), true);
  const auto thunk = *table.slot;
  require(thunk != reinterpret_cast<void*>(&original_target) && protection(table.slot) == PAGE_READWRITE,
          "Injected install restoration failure did not leave the expected state");
  reinterpret_cast<void (*)(void*) noexcept>(thunk)(table.slot);
  require(original_calls == 1 && observer_calls == 1, "A successful CAS was not callable after restoration failed");
  const auto before_remove = protection_calls;
  require_restore_failure(remove(), false);
  require(protection_calls == before_remove + 1 && *table.slot == thunk && protection(table.slot) == PAGE_READWRITE,
          "Removal exchanged a pointer while the original protection remained unresolved");
  const auto restored = restore_protection();
  require(restored.status == Status::protection_restored && !restored.pointer_changed && restored.protection_restored &&
              *table.slot == thunk && protection(table.slot) == PAGE_READONLY,
          "Explicit recovery did not restore the retained original protection");
  require(install(table.slot, reinterpret_cast<void*>(&original_target)).status == Status::installation_consumed,
          "A successful install CAS did not permanently consume installation");
  verify_removed(table);
}

void remove_restore_failure() {
  MockTable table;
  require(install(table.slot, reinterpret_cast<void*>(&original_target)).status == Status::installed, "Cannot install mock hook");
  fail_readonly = 2;
  require_restore_failure(remove(), true);
  require(*table.slot == reinterpret_cast<void*>(&original_target) && protection(table.slot) == PAGE_READWRITE,
          "Injected removal restoration failure did not leave the expected state");
  require_restore_failure(restore_protection(), false);
  const auto result = remove();
  require(result.status == Status::not_installed && !result.pointer_changed && result.protection_restored &&
              *table.slot == reinterpret_cast<void*>(&original_target) && protection(table.slot) == PAGE_READONLY,
          "Repeated removal lost the original read-only protection or repeated the pointer exchange");
}

void mismatch_restore_failure() {
  MockTable table;
  replace_after_writable = reinterpret_cast<void*>(&foreign_target);
  fail_readonly = 2;
  require_restore_failure(install(table.slot, reinterpret_cast<void*>(&original_target), &observer), false);
  require(*table.slot == reinterpret_cast<void*>(&foreign_target) && protection(table.slot) == PAGE_READWRITE,
          "Injected compare/exchange race did not preserve the foreign pointer");
  const auto before_retry = protection_calls;
  require_restore_failure(install(table.slot, reinterpret_cast<void*>(&original_target), &observer), false);
  require(protection_calls == before_retry + 1 && *table.slot == reinterpret_cast<void*>(&foreign_target),
          "Install attempted another pointer exchange while protection recovery was pending");
  const auto result = remove();
  require(result.status == Status::not_installed && !result.pointer_changed && result.protection_restored &&
              *table.slot == reinterpret_cast<void*>(&foreign_target) && protection(table.slot) == PAGE_READONLY,
          "An uninstalled CAS mismatch lost its pending protection recovery");
  table.reset_foreign_for_test();
  require(install(table.slot, reinterpret_cast<void*>(&original_target)).status == Status::installed,
          "A failed install CAS incorrectly consumed the module's installation");
  verify_removed(table);
}

void writable_failure() {
  MockTable table;
  fail_writable = 1;
  const auto result = install(table.slot, reinterpret_cast<void*>(&original_target));
  require(result.status == Status::protection_change_failed && result.windows_error == ERROR_ACCESS_DENIED && !result.pointer_changed &&
              result.protection_restored && *table.slot == reinterpret_cast<void*>(&original_target) &&
              protection(table.slot) == PAGE_READONLY,
          "A failed initial writable-protection change modified the pointer or recovery state");
  const auto before_restore = protection_calls;
  require(restore_protection().status == Status::protection_restored && protection_calls == before_restore,
          "An unchanged page incorrectly required protection recovery");
  require(install(table.slot, reinterpret_cast<void*>(&original_target)).status == Status::installed,
          "A refused writable-protection change consumed installation");
  verify_removed(table);
}

ImageDataSlotProof proof_for(void* slot) {
  const auto image = GetModuleHandleW(nullptr);
  const auto base = reinterpret_cast<std::uintptr_t>(image);
  const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
  const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  const auto sections = IMAGE_FIRST_SECTION(nt);
  const auto rva = reinterpret_cast<std::uintptr_t>(slot) - base;
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    const auto extent = sections[i].Misc.VirtualSize != 0 ? sections[i].Misc.VirtualSize : sections[i].SizeOfRawData;
    if (rva >= sections[i].VirtualAddress && rva - sections[i].VirtualAddress < extent)
      return {image, nt->OptionalHeader.SizeOfImage, sections[i].VirtualAddress, extent};
  }
  throw std::runtime_error("Validation image slot has no PE section");
}

void set_protection(void* slot, DWORD desired) {
  DWORD previous = 0;
  require(VirtualProtect(slot, sizeof(void*), desired, &previous) != FALSE, "Cannot set validation image protection");
}

void image_data_slot(bool fail_restore) {
  auto** slot = const_cast<void**>(&image_slot);
  auto proof = proof_for(slot);
  mock_slot = slot;
  const auto original_protection = protection(slot);
  require(original_protection == PAGE_READONLY, "Validation image slot must start read-only");
  set_protection(slot, PAGE_EXECUTE_WRITECOPY);
  require(protection(slot) == PAGE_EXECUTE_WRITECOPY, "Image fixture did not acquire executable copy-on-write protection");
  const auto refusals_begin = protection_calls;
  require(install(slot, reinterpret_cast<void*>(&original_target), &observer).status == Status::invalid_slot_memory,
          "Default policy admitted executable copy-on-write data");
  for (unsigned variant = 0; variant < 5; ++variant) {
    auto wrong = proof;
    if (variant == 0)
      wrong.main_image = GetModuleHandleW(L"kernel32.dll");
    else if (variant == 1)
      ++wrong.image_size;
    else if (variant == 2)
      ++wrong.section_rva;
    else if (variant == 3)
      --wrong.section_size;
    else
      wrong.section_size = 0xffffffffu;
    require(install(slot, reinterpret_cast<void*>(&original_target), &observer, &wrong).status == Status::invalid_slot_memory,
            "Invalid image data proof was accepted");
  }
  for (const auto refused : {PAGE_EXECUTE, PAGE_EXECUTE_READ}) {
    set_protection(slot, refused);
    require(install(slot, reinterpret_cast<void*>(&original_target), &observer, &proof).status == Status::invalid_slot_memory,
            "An executable protection other than copy-on-write was accepted");
  }
  set_protection(slot, PAGE_EXECUTE_WRITECOPY);
  auto writable_proof = proof_for(&writable_image_slot);
  const auto writable_previous = protection(&writable_image_slot);
  set_protection(&writable_image_slot, PAGE_EXECUTE_WRITECOPY);
  require(protection(&writable_image_slot) == PAGE_EXECUTE_WRITECOPY, "Writable PE fixture must exercise the section flag gate");
  require(install(&writable_image_slot, reinterpret_cast<void*>(&original_target), &observer, &writable_proof).status ==
              Status::invalid_slot_memory,
          "A writable PE section was accepted by image data proof");
  set_protection(&writable_image_slot, writable_previous);
  auto* executable_slot = reinterpret_cast<void*>(&executable_image_fixture);
  auto executable_proof = proof_for(executable_slot);
  const auto executable_previous = protection(executable_slot);
  set_protection(executable_slot, PAGE_EXECUTE_WRITECOPY);
  require(protection(executable_slot) == PAGE_EXECUTE_WRITECOPY, "Executable PE fixture must exercise the section flag gate");
  require(
      install(reinterpret_cast<void**>(executable_slot), reinterpret_cast<void*>(&original_target), &observer, &executable_proof).status ==
          Status::invalid_slot_memory,
      "An executable PE section was accepted by image data proof");
  set_protection(executable_slot, executable_previous);
  require(protection_calls == refusals_begin, "A refused image proof attempted a protection change");

  if (fail_restore)
    fail_exact_call = protection_calls + 2;
  auto result = install(slot, reinterpret_cast<void*>(&original_target), &observer, &proof);
  if (fail_restore) {
    require_restore_failure(result, true);
    result = restore_protection();
    require(result.status == Status::protection_restored && result.protection_restored && !result.pointer_changed,
            "Executable image protection recovery failed");
  } else {
    require(result.status == Status::installed && result.pointer_changed && result.protection_restored,
            "Verified image data slot was refused");
  }
  require(protection(slot) == PAGE_EXECUTE_READWRITE, "Windows copy-on-write promotion was not observed after the image CAS");
  const auto thunk = *static_cast<void* volatile*>(static_cast<void*>(slot));
  reinterpret_cast<void (*)(void*) noexcept>(thunk)(slot);
  require(original_calls == 1 && observer_calls == 1, "Image data hook was not callable");
  result = remove();
  require(result.status == Status::removed && result.pointer_changed && result.protection_restored &&
              *static_cast<void* volatile*>(static_cast<void*>(slot)) == reinterpret_cast<void*>(&original_target) &&
              protection(slot) == PAGE_EXECUTE_READWRITE,
          "Image data hook removal did not restore its pointer and exact executable protection");
  require(executable_protection_calls == protection_calls - refusals_begin,
          "Executable image hook temporarily removed executable permission");
  set_protection(slot, original_protection);
}

void image_private_refusal() {
  auto** slot = const_cast<void**>(&image_slot);
  const auto proof = proof_for(slot);
  mock_slot = slot;
  const auto previous = protection(slot);
  set_protection(slot, PAGE_EXECUTE_WRITECOPY);
  InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), reinterpret_cast<void*>(&original_target));
  require(protection(slot) == PAGE_EXECUTE_READWRITE, "Private image fixture did not report the promoted page protection");
  require(install(slot, reinterpret_cast<void*>(&original_target), &observer, &proof).status == Status::invalid_slot_memory &&
              protection_calls == 0,
          "Initial image admission accepted an already private RWX page");
  set_protection(slot, previous);
  MockTable table;
  set_protection(table.slot, PAGE_EXECUTE_READWRITE);
  require(install(table.slot, reinterpret_cast<void*>(&original_target), &observer, &proof).status == Status::invalid_slot_memory &&
              protection_calls == 0,
          "Image proof admitted anonymous executable memory");
}
}  // namespace

// Linked only with observer_hook.cpp compiled with TAXI_ENGINE_HOOK_VALIDATION.
// Normal production code calls VirtualProtect directly and has no test seam.
extern "C" BOOL taxi_hook_test_virtual_protect(void* address, SIZE_T size, DWORD desired, PDWORD prior) noexcept {
  ++protection_calls;
  if ((desired & 0xff) == PAGE_EXECUTE_WRITECOPY || (desired & 0xff) == PAGE_EXECUTE_READWRITE) {
    ++executable_protection_calls;
    if ((desired & PAGE_TARGETS_NO_UPDATE) == 0) {
      SetLastError(ERROR_INVALID_PARAMETER);
      return FALSE;
    }
  }
  if (address != mock_slot || size != sizeof(void*)) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  if (protection_calls == fail_exact_call) {
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }
  auto& failures = desired == PAGE_READONLY ? fail_readonly : fail_writable;
  if (failures != 0) {
    --failures;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }
  const auto changed = VirtualProtect(address, size, desired, prior);
  if (changed && desired == PAGE_READWRITE && replace_after_writable != nullptr) {
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(address), replace_after_writable);
    replace_after_writable = nullptr;
  }
  return changed;
}

int main(int argc, char** argv) {
  try {
    require(argc == 2, "Pass exactly one protection-failure scenario");
    if (std::strcmp(argv[1], "install-restore") == 0)
      install_restore_failure();
    else if (std::strcmp(argv[1], "remove-restore") == 0)
      remove_restore_failure();
    else if (std::strcmp(argv[1], "mismatch-restore") == 0)
      mismatch_restore_failure();
    else if (std::strcmp(argv[1], "writable-change") == 0)
      writable_failure();
    else if (std::strcmp(argv[1], "image-data") == 0)
      image_data_slot(false);
    else if (std::strcmp(argv[1], "image-data-restore") == 0)
      image_data_slot(true);
    else if (std::strcmp(argv[1], "image-private-refusal") == 0)
      image_private_refusal();
    else
      throw std::runtime_error("Unknown protection-failure scenario");
    std::printf("PASS: injected protection failure/recovery: %s\n", argv[1]);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
