// Diagnostic executable only. Reuse the exact same-user, main-image identity,
// code-profile and aircraft graph gates; no installed add-on or engine calls.
#include "main_module_inventory.cpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace taxi_camera::discovery {
class PoseRecorder final : public engine_camera::MemoryReader {
 public:
  PoseRecorder(HANDLE process, std::uint64_t node, std::uint64_t camera) : reader_(process), node_(node), camera_(camera) {}
  bool read(std::uint64_t address, void* out, std::size_t bytes) override {
    if (!reader_.read(address, out, bytes))
      return false;
    if (address == node_ + 296 && bytes == 8)
      std::memcpy(&matrix_, out, 8);
    if (matrix_ && address == matrix_ + 96 && bytes == 24) {
      std::memcpy(position.data(), out, 24);
      seen_ |= 1;
    }
    for (unsigned row = 0; row < 3; ++row)
      if (address == camera_ + 1648 + row * 32 && bytes == 24) {
        std::memcpy(basis[row].data(), out, 24);
        seen_ |= 2u << row;
      }
    return true;
  }
  bool complete() const { return seen_ == 15; }
  std::array<double, 3> position{};
  std::array<std::array<double, 3>, 3> basis{};
 private:
  AircraftMemoryReader reader_;
  std::uint64_t node_, camera_, matrix_ = 0;
  unsigned seen_ = 0;
};
int compare_pose(DWORD pid) {
  OwnedHandle process;
  process.value = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!process.value) { std::puts("{\"complete\":false,\"error\":\"open_process\"}"); return 1; }
  const auto result = inspect_simulator_main_module(pid, {}, false, CameraContextKind::aircraft);
  DWORD exit_code = 0;
  if (!result.main_module_verified || !result.code_contract.valid || !result.source_pose.complete ||
      !GetExitCodeProcess(process.value, &exit_code) || exit_code != STILL_ACTIVE) {
    std::printf("{\"complete\":false,\"error\":\"identity_code_or_source_gate\",\"image\":%s,\"code\":%s,\"pose\":%s,\"windows_error\":%u}\n",
      result.main_module_verified ? "true" : "false", result.code_contract.valid ? "true" : "false",
      result.source_pose.complete ? "true" : "false", unsigned(result.windows_error)); return 1;
  }
  PoseRecorder reader(process.value, result.source_pose.node_address, result.source_pose.camera_address);
  const auto pose = native_camera::inspect_source_pose(reader, result.source_pose.source_address);
  if (!pose.complete || !reader.complete() || pose.node_address != result.source_pose.node_address ||
      pose.camera_address != result.source_pose.camera_address) {
    std::puts("{\"complete\":false,\"error\":\"source_changed_or_unreadable\"}"); return 1;
  }
  std::printf("{\"complete\":true,\"pid\":%u,\"read_bytes\":%u,\"fov\":%.9g,\"position\":[%.17g,%.17g,%.17g],\"basis\":[",
    unsigned(pid), pose.read_bytes, double(pose.fov), reader.position[0], reader.position[1], reader.position[2]);
  for (unsigned row = 0; row < 3; ++row)
    std::printf("%s[%.17g,%.17g,%.17g]", row ? "," : "", reader.basis[row][0], reader.basis[row][1], reader.basis[row][2]);
  std::puts("]}");
  // Separately labelled numeric parent probe. The captured setter reads the
  // untagged parent Node pointer at +368 (not a generation handle). No ancestor
  // walk or body-identity claim: inspect this one parent using the same Node
  // matrix pointer layout and recheck every fixed read before publication.
  AircraftMemoryReader parent_reader(process.value);
  std::uint64_t parent = 0, matrix = 0, again = 0;
  std::array<std::array<double, 3>, 4> rows{};
  bool parent_ok = parent_reader.read(pose.node_address + 368, &parent, 8);
  if (parent_ok && parent && !(parent & 7) && parent <= UINT64_MAX - 304) {
    parent_ok = parent_reader.read(parent + 296, &matrix, 8) && matrix && !(matrix & 7) && matrix <= UINT64_MAX - 120;
    for (unsigned row = 0; parent_ok && row < 4; ++row) {
      parent_ok = parent_reader.read(matrix + row * 32, rows[row].data(), 24);
      for (const auto value : rows[row]) parent_ok &= std::isfinite(value);
    }
    parent_ok = parent_ok && parent_reader.read(pose.node_address + 368, &again, 8) && again == parent;
    parent_ok = parent_ok && parent_reader.read(parent + 296, &again, 8) && again == matrix;
    for (unsigned row = 0; parent_ok && row < 4; ++row) {
      std::array<double, 3> verify{};
      parent_ok = parent_reader.read(matrix + row * 32, verify.data(), 24) && verify == rows[row];
    }
    if (parent_ok) {
      std::printf("{\"parent_complete\":true,\"body_identity_verified\":false,\"read_bytes\":224,\"matrix_rows\":[");
      for (unsigned row = 0; row < 4; ++row)
        std::printf("%s[%.17g,%.17g,%.17g]", row ? "," : "", rows[row][0], rows[row][1], rows[row][2]);
      std::puts("]}");
    } else std::puts("{\"parent_complete\":false,\"error\":\"parent_matrix_unavailable_or_changed\"}");
  } else std::printf("{\"parent_complete\":%s,\"parent_present\":%s}\n", parent_ok ? "true" : "false", parent ? "true" : "false");
  return 0;
}
}  // namespace taxi_camera::discovery
int main(int argc, char** argv) {
  if (argc != 3 || std::strcmp(argv[1], "--pid")) return 2;
  char* end = nullptr;
  const auto pid = std::strtoul(argv[2], &end, 10);
  if (!pid || !end || *end || pid > UINT32_MAX) return 2;
  return taxi_camera::discovery::compare_pose(static_cast<DWORD>(pid));
}
