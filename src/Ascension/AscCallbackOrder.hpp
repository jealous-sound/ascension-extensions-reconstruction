#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace AscRuntime
{
    // Use measured native list traversal, not the new DLL's function-pointer hashes. The original
    // registrar stores callbacks in an MSVC unordered_set; neither call-site nor pointer order matches it.
    template <class Callback, size_t N>
    void InsertByCapturedOrder(std::vector<std::pair<uint32_t, Callback>>& callbacks, Callback cb,
                               uint32_t site, const uint32_t (&sites)[N])
    {
        if (std::any_of(callbacks.begin(), callbacks.end(), [cb](const auto& entry) { return entry.second == cb; }))
            return;
        callbacks.emplace_back(site, cb);
        auto rank = [&sites](uint32_t address)
        {
            return std::find(sites, sites + N, address) - sites;
        };
        std::sort(callbacks.begin(), callbacks.end(), [&rank](const auto& a, const auto& b)
        {
            auto aRank = rank(a.first), bRank = rank(b.first);
            return aRank != bRank ? aRank < bRank : a.first < b.first;
        });
    }
}
