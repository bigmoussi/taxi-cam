#pragma once

#include "image_inventory.hpp"

namespace taxi_camera::discovery {

struct LiteralTarget {
  std::string label;
  std::uint32_t rva;
  std::string expected_text;
};

struct CodeTarget {
  std::string label;
  std::uint32_t rva;
};

class InstructionDecoder {
 public:
  virtual ~InstructionDecoder() = default;
  virtual std::size_t decode(std::uint8_t* bytes, std::size_t available, std::uint32_t rva, std::string& instruction) = 0;
};

struct InstructionMetadata {
  std::uint32_t rva = 0;
  std::string text;
  std::uint32_t direct_call_target_rva = 0;
};

struct CodeReference {
  std::string target_label;
  std::uint32_t target_rva = 0;
  std::uint32_t function_begin_rva = 0;
  std::uint32_t function_end_rva = 0;
  std::uint32_t instruction_rva = 0;
  // Alignment is checked by linear LLVM decoding from the PE runtime-function
  // boundary. This is not a proof of execution, reachability or a function ABI.
  std::vector<InstructionMetadata> context;
};

struct ReferenceInventory {
  bool valid_targets = false;
  std::string error;
  // Newly scanned executable bytes, capped at 128 MiB. This excludes small
  // cross-chunk overlaps, literal/.pdata reads and candidate function reads.
  std::uint64_t code_bytes = 0;
  std::uint64_t decoded_bytes = 0;
  std::uint32_t candidate_count = 0;
  std::uint32_t missing_function_bounds = 0;
  std::uint32_t failed_boundary_checks = 0;
  std::uint32_t read_failures = 0;
  std::uint32_t runtime_function_count = 0;
  bool code_limit_reached = false;
  bool decode_limit_reached = false;
  bool reference_limit_reached = false;
  std::vector<CodeReference> references;
};

ReferenceInventory find_literal_references(ImageReader& reader,
                                           const Inventory& image,
                                           InstructionDecoder& decoder,
                                           const std::vector<LiteralTarget>& targets);

// Matches only E8 rel32 calls to one to eight explicit static executable RVAs.
// Required literals pin the image before scanning. Call sites must be decoded
// boundaries within validated runtime-function ranges; callees may be leaves
// without their own runtime-function entry. No target code is invoked.
// Limits: 128 MiB scanned code (not total reader bytes), 8 MiB decoded code,
// 32 references, 1024 candidates and 64 KiB per candidate function read.
ReferenceInventory find_direct_call_references(ImageReader& reader,
                                               const Inventory& image,
                                               InstructionDecoder& decoder,
                                               const std::vector<LiteralTarget>& required_literals,
                                               const std::vector<CodeTarget>& targets);

struct FunctionContext {
  std::uint32_t requested_rva = 0;
  std::uint32_t function_begin_rva = 0;
  std::uint32_t function_end_rva = 0;
  std::uint32_t bytes_read = 0;
  std::uint32_t decoded_bytes = 0;
  bool truncated = false;
  bool decode_failed = false;
  bool requested_boundary_found = false;
  std::string error;
  std::vector<InstructionMetadata> instructions;
};

struct FunctionInventory {
  bool valid_targets = false;
  std::string error;
  std::uint64_t code_bytes = 0;
  std::uint64_t decoded_bytes = 0;
  std::uint32_t runtime_function_count = 0;
  std::uint32_t read_failures = 0;
  std::uint32_t missing_function_bounds = 0;
  // Any requested context was incomplete or refused. Successful contexts remain.
  bool partial = false;
  std::vector<FunctionContext> functions;
};

// Reads only the validated function prefix for one to eight explicit RVAs:
// default 4096 code bytes per request; caller prefix_limit must be 1..16384.
// The 32768-byte aggregate cap is independent of request count and prefix limit.
// Linear decoded metadata is
// not a control-flow, reachability, calling-convention or execution guarantee.
FunctionInventory inspect_function_contexts(ImageReader& reader,
                                            const Inventory& image,
                                            InstructionDecoder& decoder,
                                            const std::vector<LiteralTarget>& required_literals,
                                            const std::vector<std::uint32_t>& instruction_rvas,
                                            std::uint32_t prefix_limit = 4096);

}  // namespace taxi_camera::discovery
