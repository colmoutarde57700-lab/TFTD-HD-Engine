#pragma once
#include "Logger.h"
#include "HdTraceLimiter.h"
#include <chrono>
#include <string>
#include <cstdint>
#include <mutex>

namespace OpenXcom
{
inline std::mutex &hdRouteMutex() { static std::mutex mutex; return mutex; }
inline HdTraceLimiter &hdTraceLimiter()
{
    static HdTraceLimiter limiter;
    return limiter;
}
inline std::uint64_t hdTraceMilliseconds()
{
    using Clock = std::chrono::steady_clock;
    static const auto start = Clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}
inline void hdTraceWrite(const std::string &line, bool warning)
{
    const SeverityLevel severity = warning ? LOG_WARNING : LOG_INFO;
    Log(severity) << line;
}
// Repeated routes update bounded memory counters, never the logfile.
inline void hdTraceRoute(const std::string &scope, const std::string &resource,
    const std::string &selected, const std::string &reason, bool native = false)
{
    std::string key = "scope=" + scope + " resource=" + resource +
        " selected=" + selected + " native=" + (native ? "YES" : "NO") + " reason=" + reason;
    for (char &c : key) if (c == '\n' || c == '\r') c = ' ';
    const bool problem = native || selected == "UNRESOLVED" || selected == "TRY_NEXT_PROVIDER" ||
        reason.find("invalid=") != std::string::npos;
    std::lock_guard<std::mutex> lock(hdRouteMutex());
    hdTraceLimiter().record(std::move(key), native, problem, hdTraceMilliseconds(), hdTraceWrite);
}
inline void hdTraceSummary()
{
    std::lock_guard<std::mutex> lock(hdRouteMutex());
    hdTraceLimiter().summary(hdTraceMilliseconds(), hdTraceWrite);
}
inline const char *hdProviderForPath(const std::string &path)
{
    if (path.find("/LegacyIndexed/") != std::string::npos || path.find("Bibliotheque/") == 0)
        return "PNG_LEGACY_HD";
    if (path.find("/RealHD/") != std::string::npos) return "REAL_HD";
    return "PNG_REMASTERED";
}
}
