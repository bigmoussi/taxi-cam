#pragma once
#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include "../shared/version.hpp"
#include "changelog.hpp"

namespace taxi_camera::standalone {
inline constexpr wchar_t ChangelogHost[] = L"raw.githubusercontent.com";
inline constexpr wchar_t ChangelogPath[] = L"/rthoms334/taxi-cam/main/changelog.json";
struct ChangelogFetchResult {
  bool ok{};
  std::string body;
};
// One bounded HTTPS GET from the fixed repository, off the UI thread. stop()
// closes the pending request so shutdown does not wait for network timeouts.
class ChangelogFetcher {
 public:
  ~ChangelogFetcher() { stop(); }
  bool busy() const { return busy_; }
  bool begin(HWND notify, UINT message) {
    if (busy_)
      return false;
    if (worker_.joinable())
      worker_.join();
    {
      std::lock_guard lock(mutex_);
      if (stopping_)
        return false;
      ready_ = false;
    }
    busy_ = true;
    worker_ = std::thread([this, notify, message] {
      ChangelogFetchResult result;
      result.ok = fetch(result.body);
      {
        std::lock_guard lock(mutex_);
        result_ = std::move(result);
        ready_ = true;
      }
      busy_ = false;
      PostMessageW(notify, message, 0, 0);
    });
    return true;
  }
  bool take(ChangelogFetchResult& result) {
    std::lock_guard lock(mutex_);
    if (!ready_)
      return false;
    result = std::move(result_);
    ready_ = false;
    return true;
  }
  void stop() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      if (request_) {
        WinHttpCloseHandle(request_);  // Cancels a pending synchronous call.
        request_ = nullptr;
      }
    }
    if (worker_.joinable())
      worker_.join();
  }

 private:
  bool fetch(std::string& body) {
    HINTERNET session = WinHttpOpen(L"Taxi Cam/" TAXI_CAM_VERSION_WIDE, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
      return false;
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);
    HINTERNET connection = WinHttpConnect(session, ChangelogHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", ChangelogPath, nullptr, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                   : nullptr;
    bool ok = false;
    if (request) {
      {
        std::lock_guard lock(mutex_);
        if (stopping_) {
          WinHttpCloseHandle(request);
          request = nullptr;
        } else {
          request_ = request;
        }
      }
      if (request) {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        DWORD status{}, size = sizeof(status);
        ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
             WinHttpReceiveResponse(request, nullptr) &&
             WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                                 &size, WINHTTP_NO_HEADER_INDEX) &&
             status == 200;
        char buffer[8192];
        DWORD read{};
        while (ok) {
          if (!WinHttpReadData(request, buffer, sizeof(buffer), &read) || body.size() + read > kChangelogMaxBytes)
            ok = false;
          else if (!read)
            break;
          else
            body.append(buffer, read);
        }
        std::lock_guard lock(mutex_);
        if (request_) {
          WinHttpCloseHandle(request_);
          request_ = nullptr;
        } else {
          ok = false;  // stop() closed the request.
        }
      }
    }
    if (connection)
      WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok && !body.empty();
  }

  std::atomic<bool> busy_{false};
  std::thread worker_;
  std::mutex mutex_;
  HINTERNET request_{};
  bool ready_{}, stopping_{};
  ChangelogFetchResult result_;
};
}  // namespace taxi_camera::standalone
