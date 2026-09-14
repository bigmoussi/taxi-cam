#include "callee_prefix.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace taxi_camera::discovery {
namespace {

constexpr std::uint32_t kRequired = 0x40000000 | 0x20000000;
constexpr std::uint32_t kExcluded = 0x80000000 | 0x02000000;
constexpr std::uint32_t kCallBytes = 5;
constexpr std::uint32_t kPrefixBytes = 64;
constexpr std::size_t kMaximumRequests = 8;

bool valid_sections(const Inventory& image) {
  if (image.sections.empty() || image.sections.size() > 96)
    return false;
  for (std::size_t index = 0; index < image.sections.size(); ++index) {
    const auto& section = image.sections[index];
    if (section.rva > image.image_size || section.size > image.image_size - section.rva)
      return false;
    for (std::size_t previous = 0; previous < index; ++previous) {
      const auto& other = image.sections[previous];
      if (section.size != 0 && other.size != 0 && section.rva < std::uint64_t(other.rva) + other.size &&
          other.rva < std::uint64_t(section.rva) + section.size)
        return false;
    }
  }
  return true;
}

const ImageSection* executable_section(const Inventory& image, std::uint32_t rva, std::uint32_t size) {
  if (size == 0 || rva >= image.image_size || size > image.image_size - rva)
    return nullptr;
  for (const auto& section : image.sections) {
    if ((section.flags & kRequired) == kRequired && (section.flags & kExcluded) == 0 && rva >= section.rva &&
        rva - section.rva < section.size && size <= section.size - (rva - section.rva))
      return &section;
  }
  return nullptr;
}

bool proven_call(const FunctionInventory& proof, const CalleePrefixRequest& request) {
  for (const auto& function : proof.functions) {
    if (function.truncated || function.decode_failed || !function.error.empty() || function.function_begin_rva > request.call_rva ||
        function.function_end_rva <= request.call_rva || kCallBytes > function.function_end_rva - request.call_rva ||
        std::uint64_t(request.call_rva) + kCallBytes > std::uint64_t(function.function_begin_rva) + function.decoded_bytes)
      continue;
    for (const auto& instruction : function.instructions) {
      if (instruction.rva == request.call_rva && instruction.direct_call_target_rva == request.target_rva)
        return true;
    }
  }
  return false;
}

bool readable_range(ImageReader& reader, std::uint32_t rva, std::uint32_t size) {
  std::uint32_t checked = 0;
  while (checked < size) {
    const auto window = reader.query(rva + checked, size - checked);
    if (!window.readable || window.size == 0 || window.size > size - checked)
      return false;
    checked += window.size;
  }
  return true;
}

std::int64_t call_target(const std::uint8_t* bytes, std::uint32_t call_rva) {
  std::uint32_t displacement = 0;
  for (std::uint32_t index = 0; index < 4; ++index)
    displacement |= std::uint32_t(bytes[index + 1]) << (8 * index);
  // Explicit sign extension avoids unsigned wrap and implementation-defined
  // uint32_t-to-int32_t conversion for backward calls.
  const auto signed_displacement = (displacement & 0x80000000u) != 0 ? std::int64_t(displacement) - 0x100000000LL : displacement;
  return std::int64_t(call_rva) + kCallBytes + signed_displacement;
}

template <typename Prefix, typename Result>
void inspect_target(ImageReader& reader,
                    const Inventory& image,
                    InstructionDecoder& decoder,
                    const ImageSection& storage,
                    Prefix& entry,
                    Result& result) {
  const auto count = std::min(kPrefixBytes, storage.size - (entry.entry_rva - storage.rva));
  if (!readable_range(reader, entry.entry_rva, count)) {
    ++result.read_failures;
    entry.stop_reason = "unreadable";
    entry.error = "The bounded target prefix is not in readable query windows.";
    return;
  }
  std::array<std::uint8_t, kPrefixBytes> prefix{};
  entry.bytes_read = count;
  result.read_bytes += count;
  if (!reader.read(entry.entry_rva, prefix.data(), count)) {
    ++result.read_failures;
    entry.stop_reason = "unreadable";
    entry.error = "The bounded target prefix could not be read exactly.";
    return;
  }

  while (entry.decoded_bytes < count) {
    const auto offset = entry.decoded_bytes;
    const auto rva = entry.entry_rva + offset;
    const auto available = count - offset;
    std::string text;
    const auto size = decoder.decode(prefix.data() + offset, available, rva, text);
    if (size == 0 || size > 15 || size > available || text.size() > 512) {
      entry.stop_reason = "decode_failed";
      entry.error = "Instruction decoding failed within the bounded prefix; its last instruction may be incomplete.";
      return;
    }
    std::uint32_t target = 0;
    if (prefix[offset] == 0xe8 && size == kCallBytes) {
      const auto candidate = call_target(prefix.data() + offset, rva);
      if (candidate > 0 && candidate < image.image_size && executable_section(image, static_cast<std::uint32_t>(candidate), 1))
        target = static_cast<std::uint32_t>(candidate);
    }
    entry.instructions.push_back({rva, std::move(text), target});
    entry.decoded_bytes += static_cast<std::uint32_t>(size);
    if ((prefix[offset] == 0xc3 && size == 1) || (prefix[offset] == 0xc2 && size == 3)) {
      entry.ret_observed = true;
      entry.stop_reason = "ret";
      return;
    }
  }
  entry.stop_reason = count == kPrefixBytes ? "prefix_limit" : "section_end";
}

CalleePrefix inspect_one(ImageReader& reader,
                         const Inventory& image,
                         InstructionDecoder& decoder,
                         const FunctionInventory& proof,
                         const CalleePrefixRequest& request,
                         CalleePrefixInventory& result) {
  CalleePrefix entry;
  entry.proof_call_rva = request.call_rva;
  entry.entry_rva = request.target_rva;
  entry.stop_reason = "proof_rejected";
  const auto* storage = executable_section(image, request.target_rva, 1);
  if (request.target_rva == 0 || !storage || !executable_section(image, request.call_rva, kCallBytes)) {
    entry.error = "The call or target is outside declared readable, non-writable executable image storage.";
    return entry;
  }
  if (!proven_call(proof, request)) {
    entry.error = "No exact decoded direct-call boundary proves this requested callee.";
    return entry;
  }
  if (!readable_range(reader, request.call_rva, kCallBytes)) {
    ++result.read_failures;
    entry.stop_reason = "unreadable";
    entry.error = "The proven call bytes are not in readable query windows.";
    return entry;
  }
  std::array<std::uint8_t, kCallBytes> call{};
  result.proof_bytes += kCallBytes;
  if (!reader.read(request.call_rva, call.data(), call.size())) {
    ++result.read_failures;
    entry.stop_reason = "unreadable";
    entry.error = "The proven call bytes could not be re-read exactly.";
    return entry;
  }
  if (call[0] != 0xe8 || call_target(call.data(), request.call_rva) != request.target_rva) {
    entry.stop_reason = "proof_mismatch";
    entry.error = "The current instruction is not an E8 rel32 call to the proved target.";
    return entry;
  }
  inspect_target(reader, image, decoder, *storage, entry, result);
  return entry;
}

template <typename Proof>
bool valid_prefix_proof(const Proof& proof, const Inventory& image, std::uint32_t maximum_proof_bytes) {
  if (!proof.valid || !proof.error.empty() || proof.read_failures != 0 || proof.entries.empty() ||
      proof.entries.size() > kMaximumRequests || proof.read_bytes > kMaximumRequests * kPrefixBytes ||
      proof.proof_bytes > maximum_proof_bytes)
    return false;
  for (const auto& entry : proof.entries) {
    if (!entry.error.empty() || entry.bytes_read == 0 || entry.bytes_read > kPrefixBytes || entry.decoded_bytes == 0 ||
        entry.decoded_bytes > entry.bytes_read || entry.instructions.empty() || entry.instructions.size() > kPrefixBytes ||
        !executable_section(image, entry.entry_rva, entry.bytes_read) || entry.instructions.front().rva != entry.entry_rva ||
        (entry.stop_reason != "ret" && entry.stop_reason != "prefix_limit" && entry.stop_reason != "section_end"))
      return false;
    const auto end = std::uint64_t(entry.entry_rva) + entry.decoded_bytes;
    for (std::size_t index = 0; index < entry.instructions.size(); ++index) {
      const auto& instruction = entry.instructions[index];
      const auto next = index + 1 == entry.instructions.size() ? end : entry.instructions[index + 1].rva;
      if (instruction.rva < entry.entry_rva || instruction.rva >= end || next <= instruction.rva || next > end ||
          next - instruction.rva > 15 || instruction.text.empty() || instruction.text.size() > 512)
        return false;
    }
  }
  return true;
}

template <typename Proof>
const InstructionMetadata* proven_branch(const Proof& proof, std::uint32_t rva, std::uint32_t& size) {
  for (const auto& entry : proof.entries) {
    for (std::size_t index = 0; index < entry.instructions.size(); ++index) {
      if (entry.instructions[index].rva != rva)
        continue;
      const auto next =
          index + 1 == entry.instructions.size() ? std::uint64_t(entry.entry_rva) + entry.decoded_bytes : entry.instructions[index + 1].rva;
      size = static_cast<std::uint32_t>(next - rva);
      return &entry.instructions[index];
    }
  }
  return nullptr;
}

bool branch_target(const std::uint8_t* bytes, std::uint32_t size, std::uint32_t rva, std::uint32_t target) {
  std::uint32_t offset = 0;
  std::uint32_t displacement_bytes = 0;
  if (size == 2 && (bytes[0] == 0xeb || (bytes[0] >= 0x70 && bytes[0] <= 0x7f))) {
    offset = 1;
    displacement_bytes = 1;
  } else if (size == 5 && bytes[0] == 0xe9) {
    offset = 1;
    displacement_bytes = 4;
  } else if (size == 6 && bytes[0] == 0x0f && bytes[1] >= 0x80 && bytes[1] <= 0x8f) {
    offset = 2;
    displacement_bytes = 4;
  } else {
    return false;
  }
  std::uint32_t displacement = 0;
  for (std::uint32_t index = 0; index < displacement_bytes; ++index)
    displacement |= std::uint32_t(bytes[offset + index]) << (8 * index);
  const auto bits = 8 * displacement_bytes;
  const auto signed_displacement =
      (displacement & (std::uint32_t(1) << (bits - 1))) != 0 ? std::int64_t(displacement) - (std::int64_t(1) << bits) : displacement;
  return std::int64_t(rva) + size + signed_displacement == target;
}

template <typename Proof>
BranchPrefix inspect_branch(ImageReader& reader,
                            const Inventory& image,
                            InstructionDecoder& decoder,
                            const Proof& proof,
                            const BranchPrefixRequest& request,
                            BranchPrefixInventory& result) {
  BranchPrefix entry;
  entry.proof_branch_rva = request.proof_branch_rva;
  entry.entry_rva = request.entry_rva;
  entry.stop_reason = "proof_rejected";
  const auto* storage = executable_section(image, request.entry_rva, 1);
  std::uint32_t size = 0;
  const auto* instruction = proven_branch(proof, request.proof_branch_rva, size);
  if (request.entry_rva == 0 || !storage || !instruction || (size != 2 && size != 5 && size != 6) ||
      !executable_section(image, request.proof_branch_rva, size)) {
    entry.error = "No exact eligible decoded branch boundary and static executable target prove this request.";
    return entry;
  }
  if (!readable_range(reader, request.proof_branch_rva, size)) {
    ++result.read_failures;
    entry.stop_reason = "unreadable";
    entry.error = "The proven branch bytes are not in readable query windows.";
    return entry;
  }
  std::array<std::uint8_t, 6> bytes{};
  result.proof_bytes += size;
  if (!reader.read(request.proof_branch_rva, bytes.data(), size)) {
    ++result.read_failures;
    entry.stop_reason = "unreadable";
    entry.error = "The proven branch bytes could not be re-read exactly.";
    return entry;
  }
  std::string text;
  if (!branch_target(bytes.data(), size, request.proof_branch_rva, request.entry_rva) ||
      decoder.decode(bytes.data(), size, request.proof_branch_rva, text) != size || text != instruction->text) {
    entry.stop_reason = "proof_mismatch";
    entry.error = "The current instruction does not match the proved relative branch to the requested target.";
    return entry;
  }
  inspect_target(reader, image, decoder, *storage, entry, result);
  return entry;
}

}  // namespace

CalleePrefixInventory inspect_callee_prefixes(ImageReader& reader,
                                              const Inventory& image,
                                              InstructionDecoder& decoder,
                                              const FunctionInventory& proof,
                                              const std::vector<CalleePrefixRequest>& requests) {
  CalleePrefixInventory result;
  if (!image.valid_image || image.machine != 0x8664 || image.image_size == 0 || !valid_sections(image)) {
    result.error = "Invalid loaded-image or section metadata.";
    return result;
  }
  if (requests.empty() || requests.size() > kMaximumRequests) {
    result.error = "Request one to eight explicitly proved direct callees.";
    return result;
  }
  if (!proof.valid_targets || proof.partial || !proof.error.empty() || proof.read_failures != 0 || proof.functions.empty() ||
      proof.functions.size() > kMaximumRequests) {
    result.error = "Fresh, successful and non-partial function contexts are required as call-boundary proof.";
    return result;
  }
  result.valid = true;
  result.entries.reserve(requests.size());
  for (const auto& request : requests) {
    result.entries.push_back(inspect_one(reader, image, decoder, proof, request, result));
    if (!result.entries.back().error.empty()) {
      result.valid = false;
      result.error = "One or more requested callee prefixes could not be verified.";
    }
  }
  return result;
}

BranchPrefixInventory inspect_branch_prefixes(ImageReader& reader,
                                              const Inventory& image,
                                              InstructionDecoder& decoder,
                                              const CalleePrefixInventory& proof,
                                              const std::vector<BranchPrefixRequest>& requests) {
  BranchPrefixInventory result;
  if (!image.valid_image || image.machine != 0x8664 || image.image_size == 0 || !valid_sections(image)) {
    result.error = "Invalid loaded-image or section metadata.";
    return result;
  }
  if (requests.empty() || requests.size() > kMaximumRequests) {
    result.error = "Request one to eight explicit branch targets.";
    return result;
  }
  if (!valid_prefix_proof(proof, image, kMaximumRequests * kCallBytes)) {
    result.error = "Fresh, successful callee prefixes are required as branch-boundary proof.";
    return result;
  }
  result.valid = true;
  result.entries.reserve(requests.size());
  for (const auto& request : requests) {
    result.entries.push_back(inspect_branch(reader, image, decoder, proof, request, result));
    if (!result.entries.back().error.empty()) {
      result.valid = false;
      result.error = "One or more requested branch prefixes could not be verified.";
    }
  }
  return result;
}

BranchPrefixInventory inspect_branch_prefixes(ImageReader& reader,
                                              const Inventory& image,
                                              InstructionDecoder& decoder,
                                              const BranchPrefixInventory& proof,
                                              const std::vector<BranchPrefixRequest>& requests) {
  BranchPrefixInventory result;
  if (!image.valid_image || image.machine != 0x8664 || image.image_size == 0 || !valid_sections(image)) {
    result.error = "Invalid loaded-image or section metadata.";
    return result;
  }
  if (requests.empty() || requests.size() > kMaximumRequests) {
    result.error = "Request one to eight explicit branch targets.";
    return result;
  }
  if (!valid_prefix_proof(proof, image, kMaximumRequests * 6)) {
    result.error = "Fresh, successful branch prefixes are required as branch-boundary proof.";
    return result;
  }
  result.valid = true;
  result.entries.reserve(requests.size());
  for (const auto& request : requests) {
    result.entries.push_back(inspect_branch(reader, image, decoder, proof, request, result));
    if (!result.entries.back().error.empty()) {
      result.valid = false;
      result.error = "One or more requested branch prefixes could not be verified.";
    }
  }
  return result;
}

}  // namespace taxi_camera::discovery
