#pragma once
#include <windows.h>
#include <array>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace taxi_camera::standalone {
// "What's new" notes live in changelog.json at the repository root. The
// companion fetches that file from main when the link is clicked and shows the
// entries at or below the installed version, newest first.
inline constexpr std::size_t kChangelogMaxBytes = 64 * 1024, kChangelogMaxReleases = 64, kChangelogMaxChanges = 32, kChangelogMaxText = 400;
struct ChangelogVersion {
  std::uint32_t major{}, minor{}, patch{};
  friend bool operator==(const ChangelogVersion&, const ChangelogVersion&) = default;
  friend auto operator<=>(const ChangelogVersion&, const ChangelogVersion&) = default;
};
struct ChangelogRelease {
  ChangelogVersion version;
  std::string date;  // Optional YYYY-MM-DD.
  std::vector<std::string> changes;
};

// Strict major.minor.patch with 16-bit components, matching Windows version resources.
inline bool parse_changelog_version(std::string_view text, ChangelogVersion& value) {
  std::array<std::uint32_t, 3> parts{};
  std::size_t at = 0;
  for (unsigned i = 0; i < 3; ++i) {
    if (i && (at >= text.size() || text[at++] != '.'))
      return false;
    const auto start = at;
    std::uint32_t number = 0;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
      number = number * 10 + static_cast<std::uint32_t>(text[at++] - '0');
      if (number > 65535)
        return false;
    }
    if (at == start || (at - start > 1 && text[start] == '0'))
      return false;
    parts[i] = number;
  }
  if (at != text.size())
    return false;
  value = {parts[0], parts[1], parts[2]};
  return true;
}
inline std::wstring changelog_version_text(const ChangelogVersion& v) {
  return std::to_wstring(v.major) + L"." + std::to_wstring(v.minor) + L"." + std::to_wstring(v.patch);
}

namespace changelog_detail {
struct Reader {
  std::string_view text;
  std::size_t at{};
  const char* error{};

  bool fail(const char* message) {
    if (!error)
      error = message;
    return false;
  }
  void space() {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n'))
      ++at;
  }
  bool take(char c) {
    space();
    if (at < text.size() && text[at] == c) {
      ++at;
      return true;
    }
    return false;
  }
  bool expect(char c, const char* message) { return take(c) || fail(message); }
  bool hex4(std::uint32_t& value) {
    value = 0;
    for (int i = 0; i < 4; ++i) {
      if (at >= text.size())
        return false;
      const char c = text[at++];
      value <<= 4;
      if (c >= '0' && c <= '9')
        value |= static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        value |= static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        value |= static_cast<std::uint32_t>(c - 'A' + 10);
      else
        return false;
    }
    return true;
  }
  static void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xc0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xe0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
      out += static_cast<char>(0xf0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    }
  }
  bool string(std::string& out, std::size_t limit) {
    space();
    if (at >= text.size() || text[at] != '"')
      return fail("Expected a string.");
    ++at;
    out.clear();
    while (at < text.size()) {
      const auto c = static_cast<unsigned char>(text[at++]);
      if (c == '"') {
        // Reject invalid UTF-8 here so display conversion cannot drop text.
        if (!out.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, out.data(), static_cast<int>(out.size()), nullptr, 0))
          return fail("Text is not valid UTF-8.");
        return true;
      }
      if (c < 0x20)
        return fail("Control character in text.");
      if (c != '\\') {
        out += static_cast<char>(c);
      } else {
        if (at >= text.size())
          break;
        const char e = text[at++];
        std::uint32_t cp{};
        switch (e) {
          case '"':
          case '\\':
          case '/':
            out += e;
            break;
          case 'b':
            out += '\b';
            break;
          case 'f':
            out += '\f';
            break;
          case 'n':
            out += '\n';
            break;
          case 'r':
            out += '\r';
            break;
          case 't':
            out += '\t';
            break;
          case 'u':
            if (!hex4(cp))
              return fail("Invalid \\u escape.");
            if (cp >= 0xdc00 && cp <= 0xdfff)
              return fail("Unpaired surrogate escape.");
            if (cp >= 0xd800 && cp <= 0xdbff) {
              std::uint32_t low{};
              if (at + 1 >= text.size() || text[at] != '\\' || text[at + 1] != 'u')
                return fail("Unpaired surrogate escape.");
              at += 2;
              if (!hex4(low) || low < 0xdc00 || low > 0xdfff)
                return fail("Unpaired surrogate escape.");
              cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
            }
            append_utf8(out, cp);
            break;
          default:
            return fail("Invalid escape in text.");
        }
      }
      if (out.size() > limit)
        return fail("Text is too long.");
    }
    return fail("Unterminated string.");
  }
  // Calls member(key) for each member; member consumes the value.
  template <class Member>
  bool object(Member&& member) {
    if (!expect('{', "Expected an object."))
      return false;
    if (take('}'))
      return true;
    do {
      std::string key;
      if (!string(key, 32) || !expect(':', "Expected ':'.") || !member(key))
        return false;
    } while (take(','));
    return expect('}', "Expected ',' or '}'.");
  }
  template <class Item>
  bool array(Item&& item) {
    if (!expect('[', "Expected an array."))
      return false;
    if (take(']'))
      return true;
    do {
      if (!item())
        return false;
    } while (take(','));
    return expect(']', "Expected ',' or ']'.");
  }
};
inline bool valid_date(std::string_view d) {
  if (d.size() != 10 || d[4] != '-' || d[7] != '-')
    return false;
  for (std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u})
    if (d[i] < '0' || d[i] > '9')
      return false;
  const int month = (d[5] - '0') * 10 + (d[6] - '0'), day = (d[8] - '0') * 10 + (d[9] - '0');
  return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}
}  // namespace changelog_detail

// Schema: {"releases": [{"version": "0.9.2", "date": "2026-09-23", "changes": ["..."]}]}
// Releases are newest first with strictly descending versions. Unknown keys are refused.
inline bool parse_changelog(std::string_view text, std::vector<ChangelogRelease>& releases, const char** error = nullptr) {
  changelog_detail::Reader r{text};
  std::vector<ChangelogRelease> parsed;
  const auto finish = [&](bool ok) {
    if (error)
      *error = ok ? nullptr : (r.error ? r.error : "Invalid changelog.");
    if (ok)
      releases = std::move(parsed);
    return ok;
  };
  if (text.size() > kChangelogMaxBytes)
    return finish(r.fail("Changelog is too large."));
  if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF")
    r.at = 3;
  bool have_releases = false;
  const auto release = [&] {
    if (parsed.size() >= kChangelogMaxReleases)
      return r.fail("Too many releases.");
    ChangelogRelease entry;
    bool have_version = false, have_date = false, have_changes = false;
    const bool ok = r.object([&](const std::string& key) {
      std::string value;
      if (key == "version" && !have_version) {
        have_version = true;
        return (r.string(value, 32) && parse_changelog_version(value, entry.version)) || r.fail("Version must be major.minor.patch.");
      }
      if (key == "date" && !have_date) {
        have_date = true;
        return (r.string(entry.date, 10) && changelog_detail::valid_date(entry.date)) || r.fail("Date must be YYYY-MM-DD.");
      }
      if (key == "changes" && !have_changes) {
        have_changes = true;
        return r.array([&] {
          if (entry.changes.size() >= kChangelogMaxChanges)
            return r.fail("Too many changes in one release.");
          if (!r.string(value, kChangelogMaxText))
            return false;
          if (value.find_first_not_of(" \t\r\n") == std::string::npos)
            return r.fail("Change text is empty.");
          entry.changes.push_back(value);
          return true;
        });
      }
      return r.fail("Unknown or repeated release field.");
    });
    if (!ok)
      return false;
    if (!have_version || entry.changes.empty())
      return r.fail("Each release needs a version and at least one change.");
    if (!parsed.empty() && !(entry.version < parsed.back().version))
      return r.fail("Releases must be newest first with unique versions.");
    parsed.push_back(std::move(entry));
    return true;
  };
  const bool ok = r.object([&](const std::string& key) {
    if (key != "releases" || have_releases)
      return r.fail("Unknown or repeated top-level field.");
    have_releases = true;
    return r.array(release);
  });
  if (!ok)
    return finish(false);
  r.space();
  if (r.at != text.size())
    return finish(r.fail("Unexpected text after the changelog."));
  if (parsed.empty())
    return finish(r.fail("Changelog has no releases."));
  return finish(true);
}

// main can carry notes for merged builds that are not published yet.
inline std::vector<ChangelogRelease> releases_up_to(const std::vector<ChangelogRelease>& releases, const ChangelogVersion& installed) {
  std::vector<ChangelogRelease> shown;
  for (const auto& release : releases)
    if (release.version <= installed)
      shown.push_back(release);
  return shown;
}

inline std::wstring changelog_widen(const std::string& text) {
  if (text.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0)
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
  return out;
}
// CRLF text for a multiline edit control.
inline std::wstring format_changelog(const std::vector<ChangelogRelease>& releases, const ChangelogVersion& installed) {
  if (releases.empty())
    return L"No notes are listed for Taxi Cam " + changelog_version_text(installed) + L" yet.";
  std::wstring text;
  for (const auto& release : releases) {
    if (!text.empty())
      text += L"\r\n";
    text += L"Taxi Cam " + changelog_version_text(release.version);
    if (!release.date.empty())
      text += L"  ·  " + changelog_widen(release.date);
    text += L"\r\n";
    for (const auto& change : release.changes) {
      std::wstring line;
      for (const wchar_t c : changelog_widen(change))
        if (c == L'\n')
          line += L"\r\n    ";
        else if (c != L'\r')
          line += c;
      text += L"•  " + line + L"\r\n";
    }
  }
  return text;
}

// The last installed version whose notes were read, in the global settings.ini
// so a settings reset shows the link again.
inline bool whats_new_pending(const std::wstring& directory, const ChangelogVersion& installed) {
  if (directory.empty())
    return true;
  wchar_t seen[64]{};
  GetPrivateProfileStringW(L"whats_new", L"seen", L"", seen, 64, (directory + L"\\settings.ini").c_str());
  std::string narrow;
  for (const wchar_t* c = seen; *c; ++c)
    narrow += *c < 0x80 ? static_cast<char>(*c) : '?';
  ChangelogVersion value;
  return !parse_changelog_version(narrow, value) || value != installed;
}
inline bool record_whats_new_seen(const std::wstring& directory, const ChangelogVersion& installed) {
  if (directory.empty())
    return false;
  return WritePrivateProfileStringW(L"whats_new", L"seen", changelog_version_text(installed).c_str(),
                                    (directory + L"\\settings.ini").c_str()) != FALSE;
}
}  // namespace taxi_camera::standalone
