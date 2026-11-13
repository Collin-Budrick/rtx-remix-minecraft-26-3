/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "ScenePacket.h"

namespace renderer_lab {
struct ExportOptions {
    bool allowApproximations = false;
};
struct ExportResult {
    std::filesystem::path sceneFile, gltfFile;
    std::uint64_t triangles{}, vertices{}, instances{}, materialVariants{}, textures{};
    std::vector<std::string> limitations;
};

// Writes real glTF 2.0 geometry, PBR materials, PNG/HDR textures and a Donut scene.
// Unsupported material/light semantics are rejected unless explicitly allowed.
ExportResult exportGltf(const SceneState& scene, const std::filesystem::path& directory,
                        const ExportOptions& options = {});
std::string jsonString(const std::string& value);
} // namespace renderer_lab
