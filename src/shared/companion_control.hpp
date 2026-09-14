#pragma once
#include "protocol.hpp"
namespace taxi_camera::standalone {
// Only a complete locked message replaces the cache. Contention is not OFF.
class CompanionControl {
 public:
  CompanionControl() noexcept { settings_.enabled = 0; }
  Mailbox::LockResult refresh(Mailbox& mailbox) noexcept {
    const auto result = mailbox.try_lock();
    if (result == Mailbox::LockResult::busy) {
      ++busy_reads_;
      return result;
    }
    if (result != Mailbox::LockResult::acquired) {
      invalidate();
      return result;
    }
    const auto& message = *mailbox.data();
    if (message.magic == ProtocolMagic && message.version == ProtocolVersion && message.bytes == sizeof(Shared) && message.owner_pid &&
        valid_settings(message.settings)) {
      settings_ = message.settings;
      heartbeat_ = message.owner_heartbeat;
    } else {
      invalidate();
    }
    mailbox.unlock();
    return result;
  }
  // Sample now AFTER refresh: a writer may advance the heartbeat after loop entry.
  bool connected(std::uint64_t now) const noexcept { return heartbeat_ && now >= heartbeat_ && now - heartbeat_ <= MaximumHeartbeatAgeMs; }
  const Settings& settings() const noexcept { return settings_; }
  std::uint64_t busy_reads() const noexcept { return busy_reads_; }
  static constexpr std::uint64_t MaximumHeartbeatAgeMs = 5000;

 private:
  void invalidate() noexcept {
    settings_ = {};
    settings_.enabled = 0;
    heartbeat_ = 0;
  }
  Settings settings_;
  std::uint64_t heartbeat_{}, busy_reads_{};
};
}  // namespace taxi_camera::standalone
