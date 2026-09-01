#pragma once

#include "romm/config.hpp"
#include "romm/errors.hpp"
#include "romm/status.hpp"
#include <string>
#include <functional>

namespace romm {

struct HttpResponse {
    int         statusCode   = 0;
    std::string statusText;
    std::string headersRaw;
    std::string body;
};

struct GamesPage {
    std::vector<Game> games;
    size_t offset{0};
    size_t limit{0};
    size_t total{0};
    bool totalKnown{false};
    bool hasMore{false};
};

// HTTP/JSON API client (http only; no redirects/chunked streaming).
bool fetchPlatforms(const Config& cfg, Status& status, std::string& outError, ErrorInfo* outInfo = nullptr);
bool fetchGamesForPlatform(const Config& cfg, const std::string& platformId, Status& status, std::string& outError, ErrorInfo* outInfo = nullptr);
bool fetchGamesPageForPlatform(const Config& cfg,
                               const std::string& platformId,
                               size_t offset,
                               size_t limit,
                               GamesPage& outPage,
                               std::string& outError,
                               ErrorInfo* outInfo = nullptr);
// Auth-explicit variant of fetchGamesPageForPlatform for background workers:
// takes plain strings instead of a Config so callers never dereference a
// Config captured on the UI thread. Authorization precedence matches
// appendAuthHeaders: bearer wins over Basic, else anonymous.
bool fetchPlatformRomsPageAuthed(const std::string& serverUrl,
                                 const std::string& bearerTokenOrEmpty,
                                 const std::string& basicAuthBase64OrEmpty,
                                 int timeoutSeconds,
                                 const std::string& platformId,
                                 size_t offset,
                                 size_t limit,
                                 GamesPage& outPage,
                                 std::string& outError);
bool fetchRomsIdentifiersDigest(const Config& cfg,
                                const std::string& platformId,
                                std::string& outDigest,
                                std::string& outError,
                                ErrorInfo* outInfo = nullptr);
bool fetchPlatformsIdentifiersDigest(const Config& cfg,
                                     std::string& outDigest,
                                     std::string& outError,
                                     ErrorInfo* outInfo = nullptr);
bool searchGamesRemote(const Config& cfg,
                       const std::string& platformId,
                       const std::string& query,
                       size_t limit,
                       std::vector<Game>& outGames,
                       std::string& outError,
                       ErrorInfo* outInfo = nullptr);
bool fetchBinary(const Config& cfg, const std::string& url, std::string& outData, std::string& outError, ErrorInfo* outInfo = nullptr);
bool enrichGameWithFiles(const Config& cfg, Game& g, std::string& outError, ErrorInfo* outInfo = nullptr);

// Fetch the firmware/bios file list for a platform. JSON is either a bare array
// of firmware objects or an {"items":[...]} envelope.
bool fetchFirmware(const Config& cfg,
                   const std::string& platformId,
                   std::vector<Firmware>& outFirmware,
                   std::string& outError,
                   ErrorInfo* outInfo = nullptr);

#ifdef UNIT_TEST
// Test helper: parse a firmware list body (array or {"items":[...]} envelope).
bool parseFirmwareListTest(const std::string& body,
                           std::vector<Firmware>& outFirmware,
                           std::string& err);
#endif
// Exposed for callers that make raw HTTP requests: the base64 Basic-auth
// payload ("" when no credentials are configured). Prefer appendAuthHeaders;
// consumers must prefix with "Authorization: Basic " themselves.
std::string basicAuthHeader(const Config& cfg);
// Effective bearer token for cfg: the paired device token (kept in sync with
// device_token.json via Config::apiToken), or "" when none is set.
std::string bearerToken(const Config& cfg);

// Append the Authorization header for cfg into headers: "Bearer <token>" when
// a bearer token is present, else "Basic <base64>" when credentials exist,
// else nothing. Single auth policy for every request path; consumers must not
// hand-build Authorization headers themselves.
void appendAuthHeaders(const Config& cfg,
                       std::vector<std::pair<std::string, std::string>>& headers);

// Shared URL parser (http:// and https://).
bool parseHttpUrl(const std::string& url,
                  std::string& host,
                  std::string& portStr,
                  std::string& path,
                  std::string& err);
// Exposed for tests and downloader reuse.
bool decodeChunkedBody(const std::string& body, std::string& decoded);

// Stream an HTTP request body to a sink without buffering the whole payload in memory.
// Note: In UNIT_TEST builds this is stubbed and not executed over the network.
bool httpRequestStream(const std::string& method,
                       const std::string& url,
                       const std::vector<std::pair<std::string, std::string>>& extraHeaders,
                       int timeoutSec,
                       struct HttpResponse& resp,
                       const std::function<bool(const char*, size_t)>& onData,
                       std::string& err);

#ifdef UNIT_TEST
// Test helper: parse a raw HTTP response string and stream its body to a sink.
bool httpRequestStreamMock(const std::string& rawResponse,
                           HttpResponse& resp,
                           const std::function<bool(const char*, size_t)>& onData,
                           std::string& err);
#endif

} // namespace romm
