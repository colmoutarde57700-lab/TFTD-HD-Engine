#pragma once
#include <cstdint>
#include <deque>
#include <set>
#include <string>

namespace OpenXcom
{
// Bounded once-per-run registry. Clock and output are injected for tests.
class HdTraceLimiter
{
    struct Detail { std::string key; bool warning; };
    std::set<std::string> _seen;
    std::deque<Detail> _problems, _normal;
    size_t _problemKeys = 0, _normalKeys = 0;
    std::uint64_t _window = 0, _summaryTick = 0, _reportedCalls = 0;
    unsigned _written = 0;
    bool _limited = false, _noticeSent = false;
public:
    static constexpr size_t MaxKeyBytes = 1536;
    static constexpr unsigned DetailsPerSecond = 8;
    std::uint64_t calls = 0, nativeCalls = 0, duplicateCalls = 0, omittedCalls = 0;
    size_t tracked() const { return _seen.size(); }
    size_t pending() const { return _problems.size() + _normal.size(); }
    template<typename Sink> void drain(std::uint64_t now, Sink sink)
    {
        if (now - _window >= 1000) { _window = now; _written = 0; }
        if (_limited && !_noticeSent && _written < DetailsPerSecond)
        {
            sink("[HD TRACE LIMIT] New HD details limited to 8 lines/s; duplicates silent. Pending/omitted counts in HD ROUTE TOTALS.", true);
            ++_written; _noticeSent = true;
        }
        while (_written < DetailsPerSecond && pending())
        {
            auto &queue = !_problems.empty() ? _problems : _normal;
            sink("[HD ROUTE] " + queue.front().key + " count=1", queue.front().warning);
            queue.pop_front(); ++_written;
        }
    }
    template<typename Sink> void record(std::string key, bool native, bool problem,
        std::uint64_t now, Sink sink)
    {
        ++calls; if (native) ++nativeCalls;
        if (key.size() > MaxKeyBytes) key = key.substr(0, MaxKeyBytes - 15) + "...[truncated]";
        if (_seen.count(key)) ++duplicateCalls;
        else if ((problem && _problemKeys >= 1024) || (!problem && _normalKeys >= 3072))
        {
            ++omittedCalls; _limited = true;
        }
        else
        {
            _seen.insert(key);
            if (problem) { ++_problemKeys; _problems.push_back({std::move(key), true}); }
            else { ++_normalKeys; _normal.push_back({std::move(key), false}); }
        }
        drain(now, sink);
        if (pending()) _limited = true;
    }
    template<typename Sink> void summary(std::uint64_t now, Sink sink)
    {
        drain(now, sink);
        if (now - _summaryTick < 30000 || calls == _reportedCalls) return;
        sink("[HD ROUTE TOTALS] calls=" + std::to_string(calls) +
            " nativeCalls=" + std::to_string(nativeCalls) + " duplicateCalls=" + std::to_string(duplicateCalls) +
            " tracked=" + std::to_string(tracked()) + " pendingDetails=" + std::to_string(pending()) +
            " omittedCalls=" + std::to_string(omittedCalls), omittedCalls != 0);
        _summaryTick = now; _reportedCalls = calls;
    }
};
}
