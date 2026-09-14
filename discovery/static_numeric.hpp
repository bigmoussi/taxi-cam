#pragma once

#include "image_inventory.hpp"

namespace taxi_camera::discovery {

enum class StaticNumericKind { float32, float64 };

struct StaticNumericRequest {
  std::uint32_t rva = 0;
  StaticNumericKind kind = StaticNumericKind::float64;
  std::uint32_t count = 1;
};

struct StaticNumericRecord {
  std::uint32_t rva = 0;
  StaticNumericKind kind = StaticNumericKind::float64;
  std::uint32_t count = 0;
  // Populated only after every requested scalar was read exactly and found
  // finite. Float32 converts exactly to double; float64 retains its precision.
  std::vector<double> values;
  std::string error;
  std::uint32_t bytes = 0;
};

struct StaticNumericInventory {
  bool valid = false;
  std::string error;
  // Attempted exact metadata bytes, including requests refused by query/read.
  // No request exceeding the remaining 256-byte allowance is issued.
  std::uint32_t read_bytes = 0;
  std::uint32_t read_failures = 0;
  std::vector<StaticNumericRecord> records;
};

// Reads one to eight explicitly requested numeric ranges in an AMD64 loaded
// image. Each request has one to three little-endian IEEE754 float32/float64
// scalars, and its complete range must stay within one declared readable,
// non-writable, non-executable, non-discardable section. Hard allowance256B;
// current request/type caps additionally limit a complete batch to192B.
// Failed requests retain successful peers but never expose partial values.
// No pointer interpretation, process API, arbitrary scanning or raw-byte output.
StaticNumericInventory inspect_static_numeric(ImageReader& reader,
                                              const Inventory& image,
                                              const std::vector<StaticNumericRequest>& requests);

}  // namespace taxi_camera::discovery
