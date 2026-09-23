#include "../../src/app/changelog.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
using namespace taxi_camera::standalone;
unsigned checks{};
void require(bool ok, const char* message) {
  ++checks;
  if (!ok)
    throw std::runtime_error(message);
}
bool parses(std::string_view text) {
  std::vector<ChangelogRelease> releases;
  return parse_changelog(text, releases);
}
std::string release(const std::string& version, const std::string& change = "\"Change\"") {
  return std::string("{\"version\":\"") + version + "\",\"changes\":[" + change + "]}";
}
std::string wrap(const std::string& releases) {
  return "{\"releases\":[" + releases + "]}";
}
std::string read_file(const char* path) {
  const auto file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    throw std::runtime_error("Could not open the repository changelog");
  std::string text(kChangelogMaxBytes + 1, '\0');
  DWORD bytes{};
  const bool ok = ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &bytes, nullptr);
  CloseHandle(file);
  if (!ok)
    throw std::runtime_error("Could not read the repository changelog");
  text.resize(bytes);
  return text;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    ChangelogVersion v;
    require(parse_changelog_version("0.9.38", v) && v == ChangelogVersion{0, 9, 38}, "Parse major.minor.patch");
    require(parse_changelog_version("65535.0.0", v), "Accept 16-bit components");
    for (const char* bad : {"", "1", "1.2", "1.2.3.4", "01.2.3", "1.2.x", "v1.2.3", "1.2.3-beta", "65536.0.0", "1..3", " 1.2.3"})
      require(!parse_changelog_version(bad, v), "Refuse malformed versions");
    require(ChangelogVersion{0, 10, 0} > ChangelogVersion{0, 9, 99} && ChangelogVersion{1, 0, 0} > ChangelogVersion{0, 99, 99},
            "Order versions numerically");

    std::vector<ChangelogRelease> releases;
    const char* error{};
    const std::string sample =
        "\xEF\xBB\xBF {\n \"releases\" : [\n"
        "  {\"version\": \"0.9.40\", \"date\": \"2026-09-24\", \"changes\": [\"Caf\\u00e9 \\\"quoted\\\" \\ud83d\\ude95\", "
        "\"Line\\nbreak\"]},\n"
        "  {\"changes\": [\"Older\"], \"version\": \"0.9.2\"}\n ]\n}\n";
    require(parse_changelog(sample, releases, &error) && !error, "Parse a valid changelog with BOM, whitespace and escapes");
    require(releases.size() == 2 && releases[0].version == ChangelogVersion{0, 9, 40} && releases[0].date == "2026-09-24" &&
                releases[1].date.empty() && releases[1].changes.size() == 1,
            "Keep release fields and order");
    require(releases[0].changes[0] == "Caf\xC3\xA9 \"quoted\" \xF0\x9F\x9A\x95", "Decode escapes and surrogate pairs as UTF-8");

    const auto installed = ChangelogVersion{0, 9, 39};
    const auto shown = releases_up_to(releases, installed);
    require(shown.size() == 1 && shown[0].version == ChangelogVersion{0, 9, 2}, "Hide notes newer than the installed version");
    require(releases_up_to(releases, {0, 9, 40}).size() == 2, "Show the installed version's own notes");
    const auto text = format_changelog(releases, installed);
    require(text.find(L"Taxi Cam 0.9.40  ·  2026-09-24\r\n•  Café \"quoted\" \U0001F695\r\n") == 0, "Format heading, date and bullets");
    require(text.find(L"•  Line\r\n    break\r\n") != std::wstring::npos, "Indent continuation lines with CRLF");
    require(text.find(L"\r\n\r\nTaxi Cam 0.9.2\r\n•  Older\r\n") != std::wstring::npos, "Separate releases; omit a missing date");
    require(format_changelog({}, installed) == L"No notes are listed for Taxi Cam 0.9.39 yet.", "Explain an empty list");

    require(parses(wrap(release("1.0.0"))), "Minimal release");
    const std::string refused[]{
        "",
        "[]",
        "{}",
        wrap(""),
        "{\"releases\":[]} x",
        "{\"releases\":[],\"releases\":[]}",
        "{\"other\":[]}",
        wrap(release("1.0.0") + ","),
        wrap("{\"version\":\"1.0.0\"}"),
        wrap("{\"version\":\"1.0.0\",\"changes\":[]}"),
        wrap(release("1.0.0", "\"  \"")),
        wrap(release("1.0.0", "1")),
        wrap(release("1.0")),
        wrap("{\"version\":\"1.0.0\",\"version\":\"1.0.0\",\"changes\":[\"x\"]}"),
        wrap("{\"version\":\"1.0.0\",\"changes\":[\"x\"],\"extra\":\"y\"}"),
        wrap("{\"version\":\"1.0.0\",\"date\":\"2026-13-01\",\"changes\":[\"x\"]}"),
        wrap("{\"version\":\"1.0.0\",\"date\":\"26-09-01\",\"changes\":[\"x\"]}"),
        wrap(release("1.0.0") + "," + release("1.0.0")),
        wrap(release("1.0.0") + "," + release("1.0.1")),
        wrap(release("1.0.0", "\"tab\there\"")),
        wrap(release("1.0.0", "\"bad \\x escape\"")),
        wrap(release("1.0.0", "\"lone \\ud83d surrogate\"")),
        wrap(release("1.0.0", "\"lone \\ude95 low\"")),
        wrap(release("1.0.0", "\"bad \xC3 utf8\"")),
        wrap(release("1.0.0", "\"unterminated")),
        wrap(release("1.0.0", "\"" + std::string(kChangelogMaxText + 1, 'x') + "\"")),
    };
    for (const auto& input : refused)
      require(!parses(input), "Refuse malformed or out-of-schema changelogs");
    releases = {ChangelogRelease{{9, 9, 9}, {}, {"kept"}}};
    require(!parse_changelog("{}", releases, &error) && error && releases.size() == 1 && releases[0].changes[0] == "kept",
            "A failed parse reports an error and leaves the previous result");
    require(parses(wrap(release("1.0.0", "\"" + std::string(kChangelogMaxText, 'x') + "\""))), "Accept text at the length limit");
    std::string many, too_many_changes;
    for (std::size_t i = 0; i <= kChangelogMaxReleases; ++i)
      many += (i ? "," : "") + release(std::to_string(kChangelogMaxReleases + 1 - i) + ".0.0");
    require(!parses(wrap(many)), "Refuse more releases than the bound");
    for (std::size_t i = 0; i <= kChangelogMaxChanges; ++i)
      too_many_changes += i ? ",\"x\"" : "\"x\"";
    require(!parses(wrap(release("1.0.0", too_many_changes))), "Refuse more changes than the bound");
    require(!parses(std::string(kChangelogMaxBytes + 1, ' ')), "Refuse oversized input before parsing");

    wchar_t cwd[32768]{};
    require(GetCurrentDirectoryW(32768, cwd), "Read isolated artifact root");
    const auto directory = std::wstring(cwd) + L"\\build\\changelog-test-" + std::to_wstring(GetCurrentProcessId());
    require(CreateDirectoryW(directory.c_str(), nullptr), "Create isolated settings fixture");
    const auto settings = directory + L"\\settings.ini";
    require(whats_new_pending(directory, installed), "Unread notes are pending without settings.ini");
    require(WritePrivateProfileStringW(L"aircraft", L"profile", L"3", settings.c_str()), "Seed unrelated settings");
    require(WritePrivateProfileStringW(L"whats_new", L"seen", L"garbage", settings.c_str()) && whats_new_pending(directory, installed),
            "A malformed marker keeps the link");
    require(record_whats_new_seen(directory, installed) && !whats_new_pending(directory, installed), "Reading records the version");
    require(whats_new_pending(directory, {0, 9, 40}) && whats_new_pending(directory, {0, 9, 38}),
            "Any other installed version shows the link again");
    require(GetPrivateProfileIntW(L"aircraft", L"profile", 0, settings.c_str()) == 3, "Recording keeps unrelated settings");
    require(whats_new_pending(L"", installed) && !record_whats_new_seen(L"", installed),
            "Missing settings directory fails open without writing a relative file");
    require(DeleteFileW(settings.c_str()) && RemoveDirectoryW(directory.c_str()), "Clean up only the isolated fixture");

    if (argc > 1) {
      releases.clear();
      const bool valid = parse_changelog(read_file(argv[1]), releases, &error);
      if (!valid)
        std::fprintf(stderr, "changelog.json: %s\n", error);
      require(valid, "Repository changelog.json is invalid");
    }
    std::printf("PASS What's new changelog: %u parsing, filtering, formatting and read-state checks%s.\n", checks,
                argc > 1 ? " including the repository changelog" : "");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL What's new changelog: %s\n", error.what());
    return 1;
  }
}
