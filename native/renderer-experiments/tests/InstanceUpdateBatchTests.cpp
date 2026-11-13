/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "instance_update_batch.h"
#include <array>
#include <limits>
#include <stdexcept>

static void require(bool condition) {
    if (!condition) throw std::runtime_error("Instance batch contract failed");
}

int main() {
    using namespace remixbridge;
    std::array<InstanceUpdate, 3> records{};
    for (std::size_t i = 0; i < records.size(); ++i) {
        records[i].instance = i + 1;
        records[i].mesh = 10 + i;
        records[i].doubleSided = i % 2;
        records[i].transform[0] = records[i].transform[5] = records[i].transform[10] = 1;
        records[i].transform[3] = float(i * 10);
    }
    int calls = 0;
    auto sink = [&](const InstanceUpdate& record) {
        require(record.instance == std::uint64_t(++calls));
        require(record.transform[3] == float((calls - 1) * 10));
        return 0u;
    };
    auto result = applyInstanceUpdates(records.data(), sizeof(records), 3, 99, sink);
    require(calls == 3 && result.packed() == (3ull << 32));
    require(!validInstanceUpdates(nullptr, sizeof(records), 3));
    require(!validInstanceUpdates(records.data(), -1, 3));
    require(!validInstanceUpdates(records.data(), sizeof(records), -1));
    require(!validInstanceUpdates(records.data(), sizeof(records) - 1, 3));
    require(!validInstanceUpdates(records.data(), sizeof(records), std::numeric_limits<int>::max()));

    // A malformed later row must not apply an earlier valid row.
    records[2].doubleSided = 2;
    calls = 0;
    result = applyInstanceUpdates(records.data(), sizeof(records), 3, 99, sink);
    require(result.applied == 0 && result.error == 99 && calls == 0);
    records[2].doubleSided = 0;
    records[2].transform[0] = std::numeric_limits<float>::quiet_NaN();
    require(!validInstanceUpdates(records.data(), sizeof(records), 3));
    records[2].transform[0] = 1;

    calls = 0;
    result = applyInstanceUpdates(records.data(), sizeof(records), 3, 99,
        [&](const InstanceUpdate&) { return ++calls == 2 ? 17u : 0u; });
    require(calls == 2 && result.applied == 1 && result.error == 17);
    std::array<unsigned char, sizeof(records) + 1> unaligned{};
    std::memcpy(unaligned.data() + 1, records.data(), sizeof(records));
    require(validInstanceUpdates(unaligned.data() + 1, sizeof(records), 3));
    return 0;
}
