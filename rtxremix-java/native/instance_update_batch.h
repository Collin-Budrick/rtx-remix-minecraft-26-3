/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace remixbridge {

struct InstanceUpdate {
    std::uint64_t instance, mesh;
    std::uint32_t categoryFlags, doubleSided;
    float transform[12];
};
static_assert(sizeof(InstanceUpdate) == 72, "Instance update wire layout changed");

struct InstanceBatchResult {
    std::uint32_t applied, error;

    std::uint64_t packed() const {
        return (std::uint64_t(applied) << 32) | error;
    }
};

// The Java wire format is little endian, including the twelve row-major floats.
// Validate the entire input before changing the retained native scene.
inline bool validInstanceUpdates(const void* data, std::int64_t capacity, int count) {
    if (!data || capacity < 0 || count < 0 ||
        std::uint64_t(count) > std::uint64_t(capacity) / sizeof(InstanceUpdate)) {
        return false;
    }
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (int i = 0; i < count; ++i) {
        InstanceUpdate record;
        std::memcpy(&record, bytes + std::size_t(i) * sizeof(record), sizeof(record));
        if (!record.instance || !record.mesh || record.doubleSided > 1) return false;
        for (float component : record.transform) {
            if (!std::isfinite(component)) return false;
        }
    }
    return true;
}

template<class Update>
InstanceBatchResult applyInstanceUpdates(const void* data, std::int64_t capacity, int count,
                                        std::uint32_t invalidArguments, Update&& update) {
    if (!validInstanceUpdates(data, capacity, count)) return {0, invalidArguments};
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (int i = 0; i < count; ++i) {
        InstanceUpdate record;
        std::memcpy(&record, bytes + std::size_t(i) * sizeof(record), sizeof(record));
        const auto error = static_cast<std::uint32_t>(update(record));
        if (error) return {static_cast<std::uint32_t>(i), error};
    }
    return {static_cast<std::uint32_t>(count), 0};
}

} // namespace remixbridge
