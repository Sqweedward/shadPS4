// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <set>
#include <unordered_map>
#include <utility>
#include <boost/icl/interval_map.hpp>
#include "common/alignment.h"
#include "common/types.h"

namespace VideoCore {

// Page ranges the page tracker must not write-protect, used for guest stacks. On Windows
// the access violation handler runs on the faulting thread's stack, so a write to a
// protected stack cannot be handled and kills the process. Ranges are kept per owner
// because neighbouring stacks share boundary pages and stack memory gets reused.
// Whole pages are excluded, except that the first page stays tracked when the stack only covers
// its tail: stacks grow downwards and only reach that page on overflow. No locking, callers
// synchronise access.
class TrackingExclusions {
    using Owners = std::set<const void*>;
    using CoverageMap = boost::icl::interval_map<VAddr, Owners>;
    using Interval = CoverageMap::interval_type;

public:
    explicit TrackingExclusions(u64 page_size_) : page_size{page_size_} {}

    std::pair<VAddr, VAddr> Add(const void* owner, VAddr addr, u64 size) {
        Remove(owner);
        const VAddr first_page = Common::AlignDown(addr, page_size);
        const VAddr end = Common::AlignUp(addr + size, page_size);
        const bool partial_first_page = addr != first_page && first_page + page_size < end;
        const VAddr begin = partial_first_page ? first_page + page_size : first_page;
        const Interval interval = Interval::right_open(begin, end);
        coverage.add(std::make_pair(interval, Owners{owner}));
        owners.emplace(owner, interval);
        return {begin, end};
    }

    std::pair<VAddr, VAddr> Remove(const void* owner) {
        const auto it = owners.find(owner);
        if (it == owners.end()) {
            return {0, 0};
        }
        const Interval interval = it->second;
        coverage.subtract(std::make_pair(interval, Owners{owner}));
        owners.erase(it);
        return {boost::icl::first(interval), boost::icl::last(interval) + 1};
    }

    std::pair<VAddr, VAddr> RangeOf(const void* owner) const {
        const auto it = owners.find(owner);
        if (it == owners.end()) {
            return {0, 0};
        }
        return {boost::icl::first(it->second), boost::icl::last(it->second) + 1};
    }

    void RemoveOverlapping(VAddr addr, u64 size) {
        if (owners.empty() || size == 0) {
            return;
        }
        const VAddr end = addr + size;
        Owners victims;
        for (auto it = coverage.lower_bound(Interval::right_open(addr, end));
             it != coverage.end() && boost::icl::first(it->first) < end; ++it) {
            victims.insert(it->second.begin(), it->second.end());
        }
        for (const void* owner : victims) {
            Remove(owner);
        }
    }

    bool Empty() const {
        return owners.empty();
    }

    template <typename Func>
    bool ForEachTracked(VAddr addr, u64 size, Func&& func) const {
        if (owners.empty() || size == 0) {
            return false;
        }
        const VAddr end = addr + size;
        VAddr cursor = addr;
        bool overlapped = false;
        for (auto it = coverage.lower_bound(Interval::right_open(addr, end)); it != coverage.end();
             ++it) {
            const VAddr begin = boost::icl::first(it->first);
            if (begin >= end) {
                break;
            }
            overlapped = true;
            if (begin > cursor) {
                func(cursor, begin - cursor);
            }
            cursor = std::max(cursor, boost::icl::last(it->first) + 1);
        }
        if (!overlapped) {
            return false;
        }
        if (cursor < end) {
            func(cursor, end - cursor);
        }
        return true;
    }

private:
    u64 page_size;
    CoverageMap coverage;
    std::unordered_map<const void*, Interval> owners;
};

} // namespace VideoCore
