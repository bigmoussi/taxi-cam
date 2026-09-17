#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "../../src/app/connection_recoverability.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
unsigned checks{};
void require(bool ok, const char* message) {
  ++checks;
  if (!ok)
    throw std::runtime_error(message);
}
}  // namespace

int main() {
  using namespace taxi_camera::standalone;
  try {
    wchar_t cwd[32768]{};
    require(GetCurrentDirectoryW(32768, cwd), "Read isolated fixture root");
    const auto directory = std::wstring(cwd) + L"\\build\\connection-recoverability-" + std::to_wstring(GetCurrentProcessId());
    require(CreateDirectoryW(directory.c_str(), nullptr), "Create isolated preference directory");

    require(load_auto_connect(directory), "Missing preference defaults to auto-connect on");
    require(save_auto_connect(directory, false) && !load_auto_connect(directory), "Persist auto-connect off");
    require(save_auto_connect(directory, true) && load_auto_connect(directory), "Persist auto-connect on");
    require(load_auto_connect(L""), "Empty directory fails open to the default");
    require(!save_auto_connect(L"", false), "Empty directory cannot write preference");

    require(should_attempt_connect(true, false, ConnectCommand::none), "Auto-connect attempts on first sighting");
    require(!should_attempt_connect(true, true, ConnectCommand::none), "Auto-connect does not hammer after an attempt");
    require(!should_attempt_connect(false, false, ConnectCommand::none), "Manual mode waits for Connect");
    require(should_attempt_connect(false, false, ConnectCommand::connect), "Connect command authorizes a manual attempt");
    require(should_attempt_connect(false, true, ConnectCommand::reset), "Reset authorizes another attempt");
    require(should_attempt_connect(true, true, ConnectCommand::connect), "Connect can retry after a stuck attempt");
    require(should_attempt_connect(false, false, ConnectCommand::none, true),
            "Latched manual authorization continues after Connect is consumed");
    require(!should_attempt_connect(false, true, ConnectCommand::none, true),
            "Latched manual mode still respects an in-flight attempt");
    require(fresh_load_allowed(false) && !fresh_load_allowed(true), "Fresh LoadLibrary only before a session load starts");
    require(!heartbeat_confirms_bridge(1000, 1000) && !heartbeat_confirms_bridge(999, 1000),
            "Stale or equal heartbeat cannot re-arm recovery");
    require(heartbeat_confirms_bridge(1001, 1000), "Newer heartbeat confirms bridge health after recovery");
    require(!heartbeat_confirms_bridge(0, 0), "Missing heartbeat does not confirm the bridge");

    ConnectCommandQueue queue;
    require(!queue.pending(), "Queue starts empty");
    queue.request(ConnectCommand::connect);
    require(queue.pending() && queue.take() == ConnectCommand::connect && !queue.pending(), "Connect request is consumed once");
    queue.request(ConnectCommand::connect);
    queue.request(ConnectCommand::reset);
    require(queue.take() == ConnectCommand::reset, "Reset supersedes a pending Connect");
    queue.request(ConnectCommand::reset);
    queue.request(ConnectCommand::connect);
    require(queue.take() == ConnectCommand::reset, "Connect cannot clear a pending Reset");

    require(DeleteFileW((directory + L"\\settings.ini").c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND, "Remove preference file");
    require(RemoveDirectoryW(directory.c_str()), "Remove isolated fixture");
    std::printf("PASS connection recoverability: %u preference, gate and command checks.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL connection recoverability: %s\n", error.what());
    return 1;
  }
}
