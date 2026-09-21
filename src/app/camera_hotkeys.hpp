#pragma once
#include <windows.h>
#include <commctrl.h>
#include <array>
#include <cwchar>
#include <string>

namespace taxi_camera::standalone {
// App preferences, deliberately separate from the bridge protocol and aircraft calibration.
struct CameraHotkey {
  UINT key{}, modifiers{};
  bool operator==(const CameraHotkey&) const = default;
};
using CameraHotkeys = std::array<CameraHotkey, 4>;
inline constexpr CameraHotkeys DefaultCameraHotkeys{
    {{'L', MOD_CONTROL | MOD_SHIFT}, {'R', MOD_CONTROL | MOD_SHIFT}, {'B', MOD_CONTROL | MOD_SHIFT}, {}}};
inline constexpr std::array<const wchar_t*, 4> CameraHotkeyNames{L"Left camera", L"Right camera", L"Both cameras", L"Camera"};
inline constexpr std::array<const wchar_t*, 4> CameraHotkeyKeys{L"left", L"right", L"both", L"camera"};
inline constexpr int CameraHotkeyFirstId = 700;
inline WORD hotkey_control_value(CameraHotkey chord) noexcept {
  BYTE flags{};
  if (chord.modifiers & MOD_CONTROL)
    flags |= HOTKEYF_CONTROL;
  if (chord.modifiers & MOD_ALT)
    flags |= HOTKEYF_ALT;
  if (chord.modifiers & MOD_SHIFT)
    flags |= HOTKEYF_SHIFT;
  return MAKEWORD(chord.key, flags);
}
inline CameraHotkey hotkey_from_control(WORD value) noexcept {
  const auto flags = HIBYTE(value);
  return {LOBYTE(value), UINT((flags & HOTKEYF_CONTROL ? MOD_CONTROL : 0) | (flags & HOTKEYF_ALT ? MOD_ALT : 0) |
                              (flags & HOTKEYF_SHIFT ? MOD_SHIFT : 0))};
}
inline bool valid_camera_hotkey(CameraHotkey chord) noexcept {
  if (!chord.key)
    return !chord.modifiers;
  // Keep unmodified simulator controls, Windows combinations, F12 (debugger),
  // navigation and system keys available. The editor can clear a binding with Delete.
  const bool key = (chord.key >= 'A' && chord.key <= 'Z') || (chord.key >= '0' && chord.key <= '9') ||
                   (chord.key >= VK_F1 && chord.key <= VK_F24 && chord.key != VK_F12);
  if (chord.key == VK_F4 && chord.modifiers == MOD_ALT)
    return false;
  return key && !(chord.modifiers & ~(MOD_CONTROL | MOD_ALT | MOD_SHIFT)) && (chord.modifiers & (MOD_CONTROL | MOD_ALT));
}
inline bool valid_camera_hotkeys(const CameraHotkeys& chords, std::wstring* error = nullptr) {
  for (size_t i = 0; i < chords.size(); ++i) {
    if (!valid_camera_hotkey(chords[i])) {
      if (error)
        *error = L"Use Ctrl or Alt with a letter, number or function key (except F12), or clear the shortcut.";
      return false;
    }
    for (size_t j = 0; j < i; ++j)
      if (chords[i].key && chords[i] == chords[j]) {
        if (error)
          *error = L"Each camera action needs a different shortcut. Clear any shortcut you do not need.";
        return false;
      }
  }
  return true;
}
inline bool load_camera_hotkeys(CameraHotkeys& chords, const std::wstring& directory) {
  if (directory.empty()) {
    chords = {};
    return false;
  }
  const auto path = directory + L"\\hotkeys.ini";
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    const auto error = GetLastError();
    const bool missing = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    chords = missing ? DefaultCameraHotkeys : CameraHotkeys{};
    return missing;
  }
  CameraHotkeys loaded{};
  for (size_t i = 0; i < loaded.size(); ++i) {
    wchar_t value[64]{};
    // The Camera action shipped later and has no default chord. A hotkeys.ini
    // written before that key exists stays valid, with Camera disabled.
    const bool optional = i + 1 == loaded.size();
    const auto length =
        GetPrivateProfileStringW(L"shortcuts", CameraHotkeyKeys[i], optional ? L"\x1" : L"", value, 64, path.c_str());
    if (optional && length == 1 && value[0] == 1 && value[1] == 0) {
      loaded[i] = {};
      continue;
    }
    wchar_t* end{};
    const auto packed = std::wcstoul(value, &end, 10);
    if (length >= 63 || end == value || *end || packed > 0xffff || (HIBYTE(packed) & ~(HOTKEYF_CONTROL | HOTKEYF_ALT | HOTKEYF_SHIFT))) {
      chords = {};
      return false;
    }
    loaded[i] = hotkey_from_control(static_cast<WORD>(packed));
  }
  if (!valid_camera_hotkeys(loaded)) {
    chords = {};
    return false;
  }
  chords = loaded;
  return true;
}
inline bool save_camera_hotkeys(const CameraHotkeys& chords, const std::wstring& directory) {
  if (directory.empty() || !valid_camera_hotkeys(chords))
    return false;
  const auto path = directory + L"\\hotkeys.ini", temporary = path + L".tmp";
  wchar_t value[256];
  const int count =
      std::swprintf(value, 256, L"[shortcuts]\r\nleft=%u\r\nright=%u\r\nboth=%u\r\ncamera=%u\r\n", hotkey_control_value(chords[0]),
                    hotkey_control_value(chords[1]), hotkey_control_value(chords[2]), hotkey_control_value(chords[3]));
  if (count <= 0)
    return false;
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  const wchar_t bom = 0xfeff;
  DWORD written{};
  bool ok = WriteFile(file, &bom, sizeof(bom), &written, nullptr) && written == sizeof(bom);
  const auto bytes = static_cast<DWORD>(count * sizeof(wchar_t));
  ok = ok && WriteFile(file, value, bytes, &written, nullptr) && written == bytes && FlushFileBuffers(file);
  CloseHandle(file);
  ok = ok && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  if (!ok)
    DeleteFileW(temporary.c_str());
  else
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
  return ok;
}

class CameraHotkeyRegistration {
 public:
  using Register = BOOL(WINAPI*)(HWND, int, UINT, UINT);
  using Unregister = BOOL(WINAPI*)(HWND, int);
  explicit CameraHotkeyRegistration(Register registration = RegisterHotKey, Unregister removal = UnregisterHotKey)
      : register_(registration), unregister_(removal) {}
  ~CameraHotkeyRegistration() { clear(); }
  CameraHotkeyRegistration(const CameraHotkeyRegistration&) = delete;
  CameraHotkeyRegistration& operator=(const CameraHotkeyRegistration&) = delete;
  bool configure(HWND window, const CameraHotkeys& chords, bool preview) {
    if (!valid_camera_hotkeys(chords))
      return false;
    clear();
    window_ = window;
    chords_ = chords;
    preview_ = preview;
    errors_ = {};
    for (size_t i = 0; i < chords.size(); ++i)
      if (window && chords[i].key && !preview) {
        registered_[i] = register_(window, CameraHotkeyFirstId + static_cast<int>(i), chords[i].modifiers | MOD_NOREPEAT, chords[i].key);
        if (!registered_[i]) {
          errors_[i] = GetLastError();
          if (!errors_[i])
            errors_[i] = ERROR_GEN_FAILURE;
        }
      }
    return true;
  }
  void clear() noexcept {
    for (size_t i = 0; i < registered_.size(); ++i)
      if (registered_[i])
        unregister_(window_, CameraHotkeyFirstId + static_cast<int>(i));
    registered_ = {};
    window_ = nullptr;
  }
  int action(WPARAM id, LPARAM keys) const noexcept {
    if (id < CameraHotkeyFirstId || id >= CameraHotkeyFirstId + chords_.size())
      return -1;
    const size_t i = id - CameraHotkeyFirstId;
    // Reject queued messages from bindings that have since been removed or changed.
    return registered_[i] && LOWORD(keys) == chords_[i].modifiers && HIWORD(keys) == chords_[i].key ? static_cast<int>(i) : -1;
  }
  std::wstring status(size_t i) const {
    if (!chords_[i].key)
      return L"Disabled";
    if (preview_)
      return L"Preview only — shortcut not registered";
    if (registered_[i])
      return L"Ready — works while Taxi Cam is hidden";
    if (errors_[i] == ERROR_HOTKEY_ALREADY_REGISTERED)
      return L"Unavailable — another app uses this shortcut";
    return L"Unavailable — Windows error " + std::to_wstring(errors_[i]);
  }
  bool conflicts() const noexcept {
    for (auto error : errors_)
      if (error)
        return true;
    return false;
  }

 private:
  Register register_;
  Unregister unregister_;
  HWND window_{};
  CameraHotkeys chords_{};
  std::array<bool, 4> registered_{};
  std::array<DWORD, 4> errors_{};
  bool preview_{};
};
}  // namespace taxi_camera::standalone
