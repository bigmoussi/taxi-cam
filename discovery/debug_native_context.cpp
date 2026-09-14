// Debugger-side helper only: reads an already debugger-stopped thread.
// Never loads into the target or suspends a thread. The guarded rewind restores
// the original instruction boundary after an authorized software breakpoint.
#include <windows.h>
#include <cstddef>
#include <cstdint>

static_assert(sizeof(CONTEXT) == 1232 && alignof(CONTEXT) == 16);
static_assert(offsetof(CONTEXT, ContextFlags) == 48);
static_assert(offsetof(CONTEXT, Rbx) == 144 && offsetof(CONTEXT, Rsp) == 152);
static_assert(offsetof(CONTEXT, Rsi) == 168 && offsetof(CONTEXT, Rdi) == 176);
static_assert(offsetof(CONTEXT, R10) == 200 && offsetof(CONTEXT, Rip) == 248);

struct DebugRegisters {
  std::uint64_t rip, rsp, rsi, rdi, rbx, r10;
  std::uint32_t error, flags;
};
static_assert(sizeof(DebugRegisters) == 56);

extern "C" __declspec(dllexport) BOOL read_debug_thread(DWORD expected_process, DWORD thread_id, DebugRegisters* output) {
  if (!output) return FALSE;
  *output = {};
  HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread_id);
  if (!thread) { output->error = GetLastError(); return FALSE; }
  if (GetProcessIdOfThread(thread) != expected_process) {
    output->error = ERROR_INVALID_PARAMETER;
    CloseHandle(thread);
    return FALSE;
  }
  alignas(16) CONTEXT context{};
  context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  const BOOL read = GetThreadContext(thread, &context);
  if (read) {
    *output = {context.Rip, context.Rsp, context.Rsi, context.Rdi, context.Rbx, context.R10, 0, context.ContextFlags};
  } else output->error = GetLastError();
  CloseHandle(thread);
  return read;
}

namespace {
BOOL change_rip(DWORD expected_process, DWORD thread_id, std::uint64_t expected_rip, std::uint64_t new_rip) {
  HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread_id);
  if (!thread) return FALSE;
  DWORD error = ERROR_SUCCESS;
  alignas(16) CONTEXT before{};
  before.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  if (GetProcessIdOfThread(thread) != expected_process) error = ERROR_INVALID_PARAMETER;
  else if (!GetThreadContext(thread, &before)) error = GetLastError();
  else if (before.Rip != expected_rip) error = ERROR_INVALID_STATE;
  if (!error) {
    CONTEXT requested = before;
    requested.Rip = new_rip;
    if (!SetThreadContext(thread, &requested)) error = GetLastError();
    else {
      alignas(16) CONTEXT after{};
      after.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      if (!GetThreadContext(thread, &after)) error = GetLastError();
      else {
        const auto* original_registers = &before.Rax;
        const auto* final_registers = &after.Rax;
        for (unsigned index = 0; index < 16; ++index)
          if (original_registers[index] != final_registers[index]) error = ERROR_INVALID_DATA;
        if (after.Rip != new_rip || before.EFlags != after.EFlags || before.SegCs != after.SegCs || before.SegSs != after.SegSs)
          error = ERROR_INVALID_DATA;
      }
    }
  }
  CloseHandle(thread);
  SetLastError(error);
  return error == ERROR_SUCCESS;
}
} // namespace

extern "C" __declspec(dllexport) BOOL rewind_debug_thread(DWORD expected_process, DWORD thread_id,
                                                          std::uint64_t expected_rip, std::uint64_t new_rip) {
  if (!new_rip || new_rip == UINT64_MAX || new_rip + 1 != expected_rip) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  return change_rip(expected_process, thread_id, expected_rip, new_rip);
}

#ifdef TAXI_DEBUG_CONTEXT_FIXTURE
// Compiled only into the separate disposable-host test helper. The production
// debugger helper exports no way to advance or choose an arbitrary RIP.
extern "C" __declspec(dllexport) BOOL advance_debug_fixture_thread(DWORD expected_process, DWORD thread_id,
                                                                  std::uint64_t original_rip) {
  if (!original_rip || original_rip == UINT64_MAX) return FALSE;
  return change_rip(expected_process, thread_id, original_rip, original_rip + 1);
}
#endif
