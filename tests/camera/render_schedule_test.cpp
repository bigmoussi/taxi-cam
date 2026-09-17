#include "../../src/camera/render_schedule.hpp"

#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
using taxi_camera::native_camera::RenderSchedule;
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}

void cadence(unsigned rate, unsigned feeds, std::uint64_t step) {
  RenderSchedule schedule;
  schedule.configure(rate, feeds);
  std::array<unsigned, 2> counts{};
  std::array<std::uint64_t, 2> last{};
  bool previous_on = false;
  unsigned expected_feed = 0;
  for (std::uint64_t time = 0; time < 10000; time += step) {
    const auto active = schedule.tick(time);
    require(!(active[0] && active[1]), "Both cameras were scheduled in one interval");
    const bool on = active[0] || active[1];
    require(!(on && previous_on), "An active interval was not followed by an off interval");
    previous_on = on;
    for (unsigned i = 0; i < 2; ++i) {
      if (!active[i])
        continue;
      require(i == expected_feed, "Feeds did not alternate fairly");
      expected_feed = (expected_feed + 1) % feeds;
      if (counts[i])
        require(time - last[i] >= (1000 + rate - 1) / rate, "Per-camera maximum rate exceeded");
      last[i] = time;
      ++counts[i];
    }
  }
  require(counts[0] > 0 && (feeds == 1 ? counts[1] == 0 : counts[1] > 0), "A requested feed was starved or disabled feed ran");
  require(counts[0] <= rate * 10 && counts[1] <= rate * 10, "Ten-second budget exceeded");
}

void changes_and_stalls() {
  RenderSchedule schedule;
  require(schedule.rate() == taxi_camera::kDefaultCameraRate && schedule.feeds() == 2, "Defaults changed");
  schedule.configure(0, 0);
  require(schedule.rate() == 5 && schedule.feeds() == 1, "Lower bounds were not applied");
  schedule.configure(999, 999);
  require(schedule.rate() == 60 && schedule.feeds() == 2, "Upper bounds were not applied");
  require(schedule.tick(100)[0], "Initial pulse missing");
  require(schedule.tick(10000) == std::array<bool, 2>{}, "A long stall left a camera on");
  require(schedule.tick(10000)[1], "Stalled schedule did not resume with the other feed");
  require(schedule.tick(10000) == std::array<bool, 2>{}, "Duplicate timestamp did not close pulse");
  require(schedule.tick(10000) == std::array<bool, 2>{}, "Duplicate timestamp caused a catch-up burst");
  schedule.configure(15, 1);
  require(schedule.tick(10001) == std::array<bool, 2>{}, "UI change erased live deadlines");
  require(schedule.tick(10067)[0], "Single-feed configuration did not resume");
  require(schedule.tick(1) == std::array<bool, 2>{}, "Clock reversal did not close camera");
  require(schedule.tick(2) == std::array<bool, 2>{}, "Clock reversal caused an immediate burst");
  require(schedule.tick(68)[0], "Clock reversal recovery failed");
  schedule.reset();
  require(schedule.rate() == 15 && schedule.feeds() == 1, "Lifecycle reset erased configuration");
  require(schedule.tick(std::numeric_limits<std::uint64_t>::max() - 1)[0], "Large clock value failed");
  require(schedule.tick(std::numeric_limits<std::uint64_t>::max()) == std::array<bool, 2>{}, "Large timestamp did not close pulse");
}
void live_rate_changes() {
  RenderSchedule schedule;
  require(schedule.tick(0)[0], "Initial default-rate pulse");
  require(schedule.tick(1) == std::array<bool, 2>{}, "Default-rate pulse closes");
  schedule.configure(60);
  require(schedule.tick(8) == std::array<bool, 2>{}, "60fps aggregate interval rounded up");
  require(schedule.tick(9)[1], "60fps alternate feed permitted after9ms");
  require(schedule.tick(10) == std::array<bool, 2>{}, "60fps still requires full off interval");
  require(schedule.tick(17) == std::array<bool, 2>{}, "Aggregate limit prevents early next feed");
  require(schedule.tick(18)[0], "Higher rate retains previous feed timestamp");
  schedule.configure(15);
  require(schedule.tick(19) == std::array<bool, 2>{}, "Lowering rate still closes previous pulse");
  require(schedule.tick(52) == std::array<bool, 2>{}, "Lowered rate retains per-feed cooldown");
  require(schedule.tick(76)[1], "Lowered rate resumes without resetting history");
}
void suspend_and_resume() {
  for (unsigned rate : {5u, 10u, 15u, 60u}) {
    RenderSchedule schedule;
    schedule.configure(rate);
    require(schedule.tick(1000)[0], "Initial pulse before telemetry gap");
    for (std::uint64_t time = 1001; time < 7000; ++time)
      require(schedule.tick(time, true) == std::array<bool, 2>{}, "Suspension left a gate open");
    require(schedule.tick(7000)[1], "Resume must preserve the next camera without recreating the pair");
    require(schedule.tick(7001) == std::array<bool, 2>{}, "Resume must close the pulse without a catch-up burst");
    require(schedule.tick(7002) == std::array<bool, 2>{}, "Resume must respect the aggregate rate limit");
  }
}

std::array<unsigned, 2> opportunities(unsigned rate, unsigned feeds, const std::array<unsigned, 6>& intervals) {
  RenderSchedule schedule;
  schedule.configure(rate, feeds);
  std::array<unsigned, 2> count{};
  std::array<std::uint64_t, 2> last{};
  bool previous_on = false;
  unsigned next = 0, tick = 0;
  for (std::uint64_t now = 0; now < 10000; now += intervals[tick++ % intervals.size()]) {
    const auto active = schedule.tick(now);
    const bool on = active[0] || active[1];
    require(!(on && previous_on) && !(active[0] && active[1]), "Jitter skipped a mandatory closed interval");
    previous_on = on;
    for (unsigned feed = 0; feed < 2; ++feed) {
      if (!active[feed])
        continue;
      require(feed == next, "Jitter starved or reordered feeds");
      next = (next + 1) % feeds;
      if (count[feed])
        require(now - last[feed] >= (1000 + rate - 1) / rate, "Jitter exceeded the requested per-feed budget");
      last[feed] = now;
      ++count[feed];
    }
  }
  require(count[0] <= rate * 10 && count[1] <= rate * 10, "Jitter exceeded the ten-second budget");
  return count;
}

void effective_lower_budgets() {
  // At 50 manager updates/s the old 15 and 30 settings both service a gate
  // transition on every update. Lower rates must add real closed idle updates.
  const std::array<unsigned, 6> fifty_hz{20, 20, 20, 20, 20, 20};
  const auto five = opportunities(5, 2, fifty_hz);
  const auto ten = opportunities(10, 2, fifty_hz);
  const auto fifteen = opportunities(15, 2, fifty_hz);
  require(five == std::array<unsigned, 2>{50, 50}, "5fps did not reduce the 50Hz manager workload");
  require(ten == std::array<unsigned, 2>{84, 83}, "10fps did not reduce the 50Hz manager workload");
  require(fifteen == std::array<unsigned, 2>{125, 125}, "Established 15fps cadence changed");
  require(opportunities(30, 2, fifty_hz) == fifteen, "50Hz manager saturation example changed");
  for (const auto& intervals : {std::array<unsigned, 6>{10, 10, 10, 10, 10, 10}, fifty_hz, std::array<unsigned, 6>{33, 33, 33, 33, 33, 33},
                                std::array<unsigned, 6>{8, 12, 17, 23, 10, 30}}) {
    for (unsigned feeds : {1u, 2u}) {
      const auto low = opportunities(5, feeds, intervals);
      const auto medium = opportunities(10, feeds, intervals);
      const auto original = opportunities(15, feeds, intervals);
      const auto total = [](const auto& count) { return count[0] + count[1]; };
      require(total(low) < total(medium) && total(medium) <= total(original), "Lower budgets did not reduce activation work");
      if (feeds == 1)
        require(!low[1] && !medium[1] && !original[1], "Single-feed mode activated the tail camera");
    }
  }
}

void lower_budget_changes() {
  RenderSchedule schedule;
  require(schedule.tick(1000)[0], "Initial opening before low-rate change");
  schedule.configure(5);
  require(schedule.tick(1001) == std::array<bool, 2>{}, "Lowering rate left the previous gate open");
  require(schedule.tick(1099) == std::array<bool, 2>{}, "Lowering rate erased aggregate history");
  require(schedule.tick(1100)[1], "5fps did not continue with the other feed");
  schedule.configure(10, 1);
  require(schedule.tick(1101) == std::array<bool, 2>{}, "Changing feeds left the tail open");
  require(schedule.tick(1199) == std::array<bool, 2>{}, "Feed-count change erased aggregate history");
  require(schedule.tick(1200)[0], "10fps single-feed schedule did not resume");
  schedule.configure(5, 2);
  require(schedule.tick(1201) == std::array<bool, 2>{}, "Restoring two feeds skipped closure");
  require(schedule.tick(1399) == std::array<bool, 2>{}, "Lowering rate erased the selected feed deadline");
  require(schedule.tick(1400)[0], "5fps pair did not preserve its pending feed");
  require(schedule.tick(20000) == std::array<bool, 2>{}, "Low-rate long stall did not close the active pulse");
  require(schedule.tick(20000)[1], "Low-rate stalled pair did not resume fairly");
  require(schedule.tick(20000) == std::array<bool, 2>{}, "Low-rate duplicate timestamp left a gate open");
  require(schedule.tick(20000) == std::array<bool, 2>{}, "Low-rate stall caused a catch-up burst");
}
}  // namespace

int main() {
  try {
    for (unsigned rate = 5; rate <= 60; ++rate)
      for (unsigned feeds : {1u, 2u})
        for (std::uint64_t step : {1, 5, 9, 10, 16, 20, 33, 50, 91, 250, 1000})
          cadence(rate, feeds, step);
    changes_and_stalls();
    live_rate_changes();
    suspend_and_resume();
    effective_lower_budgets();
    lower_budget_changes();
    std::printf("PASS: %u render-schedule checks; rate limits, alternating feeds, mandatory off intervals and no catch-up bursts.\n",
                checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
