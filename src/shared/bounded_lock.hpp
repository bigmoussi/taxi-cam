#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include "hook_timing.hpp"

namespace taxi_camera {

// Wait budgets for simulator-owned threads. A render, present, command-list or
// D3D12 lifetime thread may spin on a bridge lock for at most this long and must
// then skip its work for the current frame. Bridge worker threads keep ordinary
// blocking locks; only the simulator's threads are protected from being parked.
namespace wait_budget {
inline constexpr std::uint32_t recording_us = 100;   // per-command hooks
inline constexpr std::uint32_t close_us = 500;       // Close-time PFD delivery
inline constexpr std::uint32_t submit_us = 1000;     // ExecuteCommandLists ordering
inline constexpr std::uint32_t lifecycle_us = 5000;  // creation / destruction, not per frame
}  // namespace wait_budget

inline std::uint64_t bounded_lock_now_us() noexcept {
  static const std::int64_t frequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value.QuadPart ? value.QuadPart : 1;
  }();
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return static_cast<std::uint64_t>(counter.QuadPart) * 1000000ull / static_cast<std::uint64_t>(frequency);
}

// Acquires a std::mutex-like lock with try_lock/unlock, never blocking longer
// than the budget. Failure is a normal result: the caller skips this frame.
template <class Mutex>
class BoundedLock {
 public:
  BoundedLock(Mutex& mutex, std::uint32_t budget_us, std::atomic<std::uint64_t>* contended = nullptr) noexcept : mutex_(mutex) {
    if (mutex_.try_lock()) {
      owned_ = true;
      return;
    }
    const auto waited_from = hook_timing::ticks();
    if (budget_us) {
      const auto deadline = bounded_lock_now_us() + budget_us;
      unsigned spins = 0;
      do {
        if (++spins % 64 == 0)
          SwitchToThread();
        else
          YieldProcessor();
        if (mutex_.try_lock()) {
          owned_ = true;
          hook_timing::record_wait(budget_us, hook_timing::ticks() - waited_from, true);
          return;
        }
      } while (bounded_lock_now_us() < deadline);
    }
    hook_timing::record_wait(budget_us, hook_timing::ticks() - waited_from, false);
    if (contended)
      contended->fetch_add(1, std::memory_order_relaxed);
  }
  ~BoundedLock() { unlock(); }
  BoundedLock(const BoundedLock&) = delete;
  BoundedLock& operator=(const BoundedLock&) = delete;
  bool owns_lock() const noexcept { return owned_; }
  explicit operator bool() const noexcept { return owned_; }
  void unlock() noexcept {
    if (owned_) {
      owned_ = false;
      mutex_.unlock();
    }
  }
  // Transfers ownership to the caller without unlocking.
  bool release() noexcept {
    const bool owned = owned_;
    owned_ = false;
    return owned;
  }

 private:
  Mutex& mutex_;
  bool owned_ = false;
};

// Fixed-capacity single-slot-per-entry ring for deferring work that could not
// take its lock on a simulator thread. Producers never block; the consumer drains
// under the lock the producer could not take. Overflow is reported so the
// consumer can fall back to a conservative global invalidation.
template <class T, unsigned Capacity>
class DeferredRing {
 public:
  bool push(const T& value) noexcept {
    auto ticket = tail_.load(std::memory_order_acquire);
    for (;;) {
      if (ticket - head_.load(std::memory_order_acquire) >= Capacity) {
        overflow_.store(true, std::memory_order_release);
        return false;
      }
      if (tail_.compare_exchange_weak(ticket, ticket + 1, std::memory_order_acq_rel, std::memory_order_acquire))
        break;
    }
    auto& slot = slots_[ticket % Capacity];
    slot.value = value;
    slot.ready.store(ticket + 1, std::memory_order_release);
    return true;
  }
  // Consumer only. Returns false when no complete entry is available.
  bool pop(T& value) noexcept {
    const auto ticket = head_.load(std::memory_order_acquire);
    if (ticket == tail_.load(std::memory_order_acquire))
      return false;
    auto& slot = slots_[ticket % Capacity];
    if (slot.ready.load(std::memory_order_acquire) != ticket + 1)
      return false;  // Producer still writing; drain it on the next pass.
    value = slot.value;
    slot.ready.store(0, std::memory_order_release);
    head_.store(ticket + 1, std::memory_order_release);
    return true;
  }
  bool take_overflow() noexcept { return overflow_.exchange(false, std::memory_order_acq_rel); }
  bool empty() const noexcept { return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire); }

 private:
  struct Slot {
    std::atomic<std::uint64_t> ready{0};
    T value{};
  };
  Slot slots_[Capacity]{};
  std::atomic<std::uint64_t> head_{0}, tail_{0};
  std::atomic<bool> overflow_{false};
};

}  // namespace taxi_camera
