#pragma once

#include "reference_inventory.hpp"

namespace taxi_camera::discovery {

struct CalleePrefixRequest {
  std::uint32_t call_rva = 0;
  std::uint32_t target_rva = 0;
};

struct CalleePrefix {
  std::uint32_t proof_call_rva = 0;
  std::uint32_t entry_rva = 0;
  // Attempted target-prefix bytes, including a failed exact read. A successful
  // read can include bytes after the first instruction-aligned RET.
  std::uint32_t bytes_read = 0;
  std::uint32_t decoded_bytes = 0;
  bool ret_observed = false;
  std::string stop_reason;
  std::string error;
  std::vector<InstructionMetadata> instructions;
};

struct CalleePrefixInventory {
  bool valid = false;
  std::string error;
  // Attempted prefix bytes (maximum 512) and separate E8 proof bytes (maximum
  // 40). Read/query failures stop that entry; successful peers are retained.
  std::uint32_t read_bytes = 0;
  std::uint32_t proof_bytes = 0;
  std::uint32_t read_failures = 0;
  std::vector<CalleePrefix> entries;
};

// Inspect one to eight direct callees proved by freshly captured function
// contexts. Re-read each exact E8 rel32 instruction before reading its target.
// Both ranges must remain in declared readable, non-writable, non-discardable
// executable main-image sections. Read at most 64 target bytes, clipped to the
// same section/image; decode linearly and stop at aligned C3 or C2 RET, or the
// read boundary. Never follow branches, inspect live objects or execute code.
// A RET or prefix boundary is NOT proof of logical function extent, execution,
// reachability, calling convention or lifetime. There is no function-end field.
CalleePrefixInventory inspect_callee_prefixes(ImageReader& reader,
                                              const Inventory& image,
                                              InstructionDecoder& decoder,
                                              const FunctionInventory& proof,
                                              const std::vector<CalleePrefixRequest>& requests);

struct BranchPrefixRequest {
  std::uint32_t proof_branch_rva = 0;
  std::uint32_t entry_rva = 0;
};

struct BranchPrefix {
  std::uint32_t proof_branch_rva = 0;
  std::uint32_t entry_rva = 0;
  std::uint32_t bytes_read = 0;
  std::uint32_t decoded_bytes = 0;
  bool ret_observed = false;
  std::string stop_reason;
  std::string error;
  std::vector<InstructionMetadata> instructions;
};

struct BranchPrefixInventory {
  bool valid = false;
  std::string error;
  // Attempted prefix bytes (maximum 512) and separate exact branch-proof bytes
  // (maximum 48). Successful peers remain when another request fails.
  std::uint32_t read_bytes = 0;
  std::uint32_t proof_bytes = 0;
  std::uint32_t read_failures = 0;
  std::vector<BranchPrefix> entries;
};

// Inspect exactly the explicitly requested single branch edge from a fresh,
// successful callee-prefix capture. Only unprefixed E9/EB JMP and 70..7F or
// 0F80..0F8F Jcc encodings are accepted. Re-read and decode the exact 2/5/6-byte
// proven instruction, require matching decoded text and the requested current
// target, then inspect at most 64 target bytes under the same rules above.
// No automatic recursive following, process access, execution or function-
// extent claim. Each invocation has its own 512 target/48 proof-byte caps.
BranchPrefixInventory inspect_branch_prefixes(ImageReader& reader,
                                              const Inventory& image,
                                              InstructionDecoder& decoder,
                                              const CalleePrefixInventory& proof,
                                              const std::vector<BranchPrefixRequest>& requests);

// Follow one further explicitly requested edge from fresh, successful branch
// prefixes, with the same boundary/opcode/text/target checks and independent
// byte caps. The prior result is consumed directly, without converting it into
// callee proof. No implicit traversal or automatic continuation is performed.
BranchPrefixInventory inspect_branch_prefixes(ImageReader& reader,
                                              const Inventory& image,
                                              InstructionDecoder& decoder,
                                              const BranchPrefixInventory& proof,
                                              const std::vector<BranchPrefixRequest>& requests);

}  // namespace taxi_camera::discovery
