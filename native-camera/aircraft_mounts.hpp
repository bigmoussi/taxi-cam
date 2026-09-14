#pragma once

#include <array>
#include <cmath>

namespace taxi_camera::native_camera {

using Vector3 = std::array<double, 3>;

// The caller establishes the aircraft datum, world units and current body
// orientation. This helper cannot derive them from a pilot camera pose.
struct BodyPose {
  Vector3 origin{};
  Vector3 right{1, 0, 0};
  Vector3 up{0, 1, 0};
  Vector3 forward{0, 0, 1};
};

struct MountConfig {
  // Metres relative to the aircraft datum: right, up, forward.
  Vector3 position_m{};
  // Local viewing convention: positive pitch looks up; positive yaw right.
  // These values are not cameras.cfg InitialPbh values.
  double pitch_degrees = 0;
  double yaw_degrees = 0;
  float fov_radians = 0.85f;
};

using MountPair = std::array<MountConfig, 2>;

struct MountedPose {
  Vector3 position{};
  Vector3 target{};
  Vector3 up{};
  float fov = 0;
};

inline constexpr float kMountMinimumFov = 0.05f;
inline constexpr float kMountMaximumFov = 1.55f;
inline constexpr double kMountMaximumOffset = 500;
inline constexpr double kMountMaximumPitch = 89;
inline constexpr double kMountMaximumYaw = 180;

namespace mount_detail {
inline bool finite(const Vector3& v) noexcept {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
inline double dot(const Vector3& a, const Vector3& b) noexcept {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline Vector3 cross(const Vector3& a, const Vector3& b) noexcept {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline Vector3 rotate(const BodyPose& body, const Vector3& v) noexcept {
  Vector3 result{};
  for (unsigned i = 0; i < result.size(); ++i)
    result[i] = body.right[i] * v[0] + body.up[i] * v[1] + body.forward[i] * v[2];
  return result;
}
}  // namespace mount_detail

inline bool valid_body_pose(const BodyPose& body) noexcept {
  using namespace mount_detail;
  if (!finite(body.origin) || !finite(body.right) || !finite(body.up) || !finite(body.forward))
    return false;
  constexpr double tolerance = 0.001;
  // Semantic right/up/forward can have either handedness in the destination
  // coordinate system. Reject nonorthogonal bases, not an ECEF axis convention.
  return std::abs(dot(body.right, body.right) - 1) <= tolerance && std::abs(dot(body.up, body.up) - 1) <= tolerance &&
         std::abs(dot(body.forward, body.forward) - 1) <= tolerance && std::abs(dot(body.right, body.up)) <= tolerance &&
         std::abs(dot(body.right, body.forward)) <= tolerance && std::abs(dot(body.up, body.forward)) <= tolerance &&
         std::abs(std::abs(dot(cross(body.right, body.up), body.forward)) - 1) <= tolerance;
}

inline bool valid_mount(const MountConfig& mount) noexcept {
  if (!mount_detail::finite(mount.position_m) || !std::isfinite(mount.pitch_degrees) || !std::isfinite(mount.yaw_degrees) ||
      !std::isfinite(mount.fov_radians) || std::abs(mount.pitch_degrees) > kMountMaximumPitch ||
      std::abs(mount.yaw_degrees) > kMountMaximumYaw || mount.fov_radians < kMountMinimumFov || mount.fov_radians > kMountMaximumFov)
    return false;
  for (double component : mount.position_m)
    if (std::abs(component) > kMountMaximumOffset)
      return false;
  return true;
}

inline bool valid_mounts(const MountPair& mounts) noexcept {
  return valid_mount(mounts[0]) && valid_mount(mounts[1]);
}

inline MountPair default_mounts() noexcept {
  // Visually calibrated starting points, not surveyed camera mounts.
  // common/config/flight_model.cfg point.19 gives the belly landmark
  // (0,-1.615317,26.950669)m and point.0 the wheel (0,-4.596384,30.220920)m.
  // Retain the position actually verified live against the user's reference.
  // Pitch/lens were tuned to retain the upright strut, put the wheel around
  // 65-70% down the pane and leave pavement below. The aft candidate was not
  // applied successfully in the live UI and is intentionally not the default.
  constexpr Vector3 nose{0, -1.75, 26.950668984};
  // Point.14 places the rudder tip at (0,20.268181,-37.572696)m. The previous
  // live position -24 cleared the fin. Move one metre aft and widen the lens
  // slightly to reduce the aircraft's size relative to the fixed reference guides.
  // This is a visual framing adjustment, not a surveyed mount or VC conversion.
  constexpr Vector3 tail{0, 18, -25};
  // The user requested a little more underbody in frame after the 1.124-radian
  // live test; the subsequent live view retained this modest 1.24 widening.
  return {{{nose, -17.5, 0, 1.24f}, {tail, -32, 0, 1.02f}}};
}

inline bool make_mounted_pose(const BodyPose& body, const MountConfig& mount, MountedPose& output) noexcept {
  output = {};
  if (!valid_body_pose(body) || !valid_mount(mount))
    return false;
  constexpr double radians = 3.14159265358979323846 / 180.0;
  const double pitch = mount.pitch_degrees * radians, yaw = mount.yaw_degrees * radians;
  const double cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw);
  const Vector3 direction = mount_detail::rotate(body, {sy * cp, sp, cy * cp});
  MountedPose result;
  result.position = mount_detail::rotate(body, mount.position_m);
  result.up = mount_detail::rotate(body, {-sy * sp, cp, -cy * sp});
  for (unsigned i = 0; i < result.position.size(); ++i) {
    result.position[i] += body.origin[i];
    result.target[i] = result.position[i] + direction[i];
  }
  result.fov = mount.fov_radians;
  if (!mount_detail::finite(result.position) || !mount_detail::finite(result.target) || !mount_detail::finite(result.up))
    return false;
  // Refuse origins so large that a unit look-at direction loses precision.
  Vector3 represented_direction{};
  for (unsigned i = 0; i < represented_direction.size(); ++i)
    represented_direction[i] = result.target[i] - result.position[i] - direction[i];
  if (mount_detail::dot(represented_direction, represented_direction) > 1e-8)
    return false;
  output = result;
  return true;
}

inline bool make_mounted_pair(const BodyPose& body, const MountPair& mounts, std::array<MountedPose, 2>& output) noexcept {
  output = {};
  std::array<MountedPose, 2> result{};
  if (!make_mounted_pose(body, mounts[0], result[0]) || !make_mounted_pose(body, mounts[1], result[1]))
    return false;
  output = result;
  return true;
}

}  // namespace taxi_camera::native_camera
