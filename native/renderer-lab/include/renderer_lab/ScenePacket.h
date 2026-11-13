/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace renderer_lab {
using Id = std::uint64_t;
using Matrix34 = std::array<float, 12>;

struct Vertex {
    std::array<float, 3> position{}, normal{};
    std::array<float, 2> uv{};
    std::uint32_t argb = 0xffffffff;
    std::array<std::uint32_t, 2> texcoordBounds{};
};
struct Texture {
    Id id{}, hash{};
    std::uint32_t width{}, height{}, format{}, purpose{};
    std::vector<std::byte> pixels;
    std::filesystem::path file;
};
struct Material {
    Id id{}, hash{};
    std::uint32_t kind{}, flags{}, alphaMode{};
    std::array<Id, 8> textures{};
    std::array<float, 4> baseColor{1, 1, 1, 1};
    std::array<float, 3> emissiveColor{};
    float emissiveIntensity{}, roughness = 1, metallic{}, ior = 1.5f, alphaCutoff = .5f;
    std::string metadata;
};
struct Surface {
    Id material{};
    std::uint32_t flags{}, bonesPerVertex{};
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices, blendIndices;
    std::vector<float> weights;
};
struct Mesh {
    Id id{}, hash{};
    std::vector<Surface> surfaces;
};
struct Instance {
    Id id{}, mesh{};
    std::uint32_t categoryFlags{};
    bool doubleSided{};
    Matrix34 transform{};
    std::vector<Matrix34> bones, gpuTransforms;
    std::string blendMetadata;
};
struct Camera {
    std::uint32_t type{}, flags{}, width{}, height{};
    std::array<float, 16> view{}, projection{};
    std::array<float, 3> position{}, forward{0, 0, -1}, up{0, 1, 0}, right{1, 0, 0};
    float fovYDegrees = 70, aspect = 16.f / 9, zNear = .05f, zFar = 10000;
};
struct Light {
    Id id{}, hash{}, colorTexture{};
    std::uint32_t type{}, flags{};
    std::array<float, 3> radiance{}, position{}, direction{}, xAxis{}, yAxis{};
    std::array<float, 4> size{};
    Matrix34 transform{};
    std::string metadata;
};

// Retained resource state shared by all experimental renderers. No SDK or Minecraft types.
class SceneState {
public:
    static constexpr std::uint32_t Magic = 0x424c5352;
    static constexpr std::uint32_t Version = 1;
    static constexpr std::uint32_t FullReset = 1;
    static constexpr std::uint32_t Incomplete = 2;
    void apply(std::span<const std::byte> packets);
    void load(const std::filesystem::path& packetFile);
    void validate() const;

    std::map<Id, Texture> textures;
    std::map<Id, Material> materials;
    std::map<Id, Mesh> meshes;
    std::map<Id, Instance> instances;
    std::map<Id, Light> lights;
    Camera camera;
    bool hasCamera{}, incomplete{};
    std::uint64_t frameId{};
};
} // namespace renderer_lab
