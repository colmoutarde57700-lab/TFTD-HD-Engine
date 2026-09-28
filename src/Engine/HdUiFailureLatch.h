#pragma once
#include <cstdint>
#include <string>

namespace OpenXcom
{
// Per-widget lifetime, never a global table indexed by reusable addresses.
class HdUiFailureLatch
{
    std::uint64_t _generation = 0;
    std::string _reason;
public:
    bool failed(std::uint64_t generation) const
    { return !_reason.empty() && generation == _generation; }
    void record(std::uint64_t generation, const std::string &reason)
    { _generation = generation; _reason = reason.empty() ? "HD presentation failed" : reason; }
};
}
