#pragma once
#include <array>
#include <cstdint>
#include <string_view>
namespace taxi_camera::profiles {
enum class DisplayLayout { a380_upper_pfd };
struct AircraftProfile {
  std::uint32_t id;
  std::string_view key;
  const wchar_t* name;
  DisplayLayout layout;
  std::array<const char*, 2> taxi_lvars;
  std::array<const char*, 2> taxi_events;
  std::array<const char*, 2> pfd_labels;
  // right/up/forward metres, pitch/yaw degrees, lens radians.
  std::array<std::array<double, 6>, 2> mounts;
  unsigned width, height, mips, nose_height, tail_height;
};
inline constexpr AircraftProfile A380{1,
                                      "fbw-a380x",
                                      L"FlyByWire A380X",
                                      DisplayLayout::a380_upper_pfd,
                                      {"L:A32NX_FCU_EFIS_L_TAXI_LIGHT_ON", "L:A32NX_FCU_EFIS_R_TAXI_LIGHT_ON"},
                                      {"A32NX.FCU_EFIS_L_TAXI_PUSH", "A32NX.FCU_EFIS_R_TAXI_PUSH"},
                                      {"SCREEN_DU_PFDL", "SCREEN_DU_PFDR"},
                                      {{{0, -1.75, 26.950668984, -17.5, 0, 1.24}, {0, 18, -25, -32, 0, 1.02}}},
                                      768,
                                      1024,
                                      5,
                                      255,
                                      504};
inline constexpr std::array<const AircraftProfile*, 1> Catalog{&A380};
inline const AircraftProfile* find(std::uint32_t id) noexcept {
  for (auto* p : Catalog)
    if (p->id == id)
      return p;
  return nullptr;
}
// Each native bridge session has one aircraft adapter. Only this verified
// adapter currently ships; adding an aircraft means adding a profile and its
// tested layout/identity strategy, not silently guessing external identifiers.
inline const AircraftProfile& active() noexcept {
  return A380;
}
}  // namespace taxi_camera::profiles
