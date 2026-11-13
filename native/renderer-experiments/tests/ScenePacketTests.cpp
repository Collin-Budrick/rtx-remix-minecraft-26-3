/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <renderer_lab/ScenePacket.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace renderer_lab;
using Bytes = std::vector<std::byte>;
struct Record { std::uint32_t operation; Bytes bytes; };

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void u32(Bytes& bytes, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(std::byte((value >> (8 * i)) & 255));
}
void u64(Bytes& bytes, std::uint64_t value) { u32(bytes, std::uint32_t(value)); u32(bytes, std::uint32_t(value >> 32)); }
void f32(Bytes& bytes, float value) { u32(bytes, std::bit_cast<std::uint32_t>(value)); }
template<std::size_t N> void floats(Bytes& bytes, const std::array<float, N>& value) {
    for (float component : value) f32(bytes, component);
}
Bytes idRecord(Id id) { Bytes bytes; u64(bytes, id); return bytes; }

Bytes packet(std::uint32_t flags, std::uint64_t frame, const std::vector<Record>& records) {
    Bytes payload;
    for (const auto& record : records) {
        u32(payload, record.operation); u32(payload, std::uint32_t(record.bytes.size()));
        payload.insert(payload.end(), record.bytes.begin(), record.bytes.end());
    }
    Bytes bytes;
    u32(bytes, SceneState::Magic); u32(bytes, SceneState::Version);
    u32(bytes, flags); u32(bytes, std::uint32_t(records.size()));
    u64(bytes, frame); u32(bytes, std::uint32_t(payload.size())); u32(bytes, 0);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}
Camera camera(float x) {
    Camera value;
    value.width = 640; value.height = 360;
    value.position = {x, 2, 8};
    for (int i = 0; i < 4; ++i) { value.view[i * 5] = 1; value.projection[i * 5] = 1; }
    return value;
}
Bytes cameraRecord(float x) {
    const auto value = camera(x);
    Bytes bytes;
    u32(bytes, value.type); u32(bytes, value.flags); u32(bytes, value.width); u32(bytes, value.height);
    floats(bytes, value.view); floats(bytes, value.projection);
    floats(bytes, value.position); floats(bytes, value.forward); floats(bytes, value.up); floats(bytes, value.right);
    f32(bytes, value.fovYDegrees); f32(bytes, value.aspect); f32(bytes, value.zNear); f32(bytes, value.zFar);
    return bytes;
}
Bytes textureRecord(Id id, std::uint32_t width, unsigned char first) {
    Bytes bytes;
    u64(bytes, id); u64(bytes, 999);
    u32(bytes, width); u32(bytes, 1); u32(bytes, 0); u32(bytes, 0); u32(bytes, width * 4);
    for (std::uint32_t i = 0; i < width * 4; ++i) bytes.push_back(std::byte(first + i));
    return bytes;
}
Bytes patchRecord(Id id, std::uint32_t x, unsigned char first) {
    Bytes bytes;
    u64(bytes, id); u32(bytes, x); u32(bytes, 0); u32(bytes, 1); u32(bytes, 1);
    u32(bytes, 4); u32(bytes, 0); u32(bytes, 4);
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(std::byte(first + i));
    return bytes;
}
Bytes meshRecord(Id id) {
    Bytes bytes;
    u64(bytes, id); u64(bytes, 999); u32(bytes, 1);
    u64(bytes, 12); u32(bytes, 3); u32(bytes, 3); u32(bytes, 64);
    for (unsigned i = 0; i < 5; ++i) u32(bytes, 0);
    for (unsigned vertex = 0; vertex < 3; ++vertex) {
        f32(bytes, float(vertex)); f32(bytes, 0); f32(bytes, 0);
        f32(bytes, 0); f32(bytes, 1); f32(bytes, 0);
        f32(bytes, 0); f32(bytes, 0); u32(bytes, 0xffffffff);
        for (unsigned i = 0; i < 7; ++i) u32(bytes, 0);
    }
    u32(bytes, 0); u32(bytes, 1); u32(bytes, 2);
    return bytes;
}

SceneState retainedScene() {
    SceneState scene;
    scene.frameId = 17; scene.incomplete = true; scene.hasCamera = true; scene.camera = camera(4);
    Texture texture;
    texture.id = 11; texture.hash = 111; texture.width = 2; texture.height = 1;
    for (unsigned i = 0; i < 8; ++i) texture.pixels.push_back(std::byte(i));
    scene.textures.emplace(texture.id, std::move(texture));
    Material material; material.id = 12; material.textures[0] = 11;
    scene.materials.emplace(material.id, std::move(material));
    Mesh mesh; mesh.id = 21; mesh.hash = 211;
    Surface surface; surface.material = 12; surface.vertices.resize(4096); surface.indices = {0, 1, 2};
    mesh.surfaces.push_back(std::move(surface)); scene.meshes.emplace(mesh.id, std::move(mesh));
    Instance instance; instance.id = 31; instance.mesh = 21;
    scene.instances.emplace(instance.id, std::move(instance));
    Light light; light.id = 41; scene.lights.emplace(light.id, std::move(light));
    return scene;
}

struct Footprint {
    const Vertex* vertices;
    const std::byte* pixels;
    Bytes contents;
};
Footprint footprint(const SceneState& scene) {
    return {scene.meshes.at(21).surfaces[0].vertices.data(), scene.textures.at(11).pixels.data(), scene.textures.at(11).pixels};
}
void unchanged(const SceneState& scene, const Footprint& before) {
    require(scene.frameId == 17 && scene.incomplete && scene.hasCamera && scene.camera.position[0] == 4,
        "Rejected input changed frame/camera/completeness state");
    require(scene.textures.size() == 1 && scene.materials.size() == 1 && scene.meshes.size() == 1
        && scene.instances.size() == 1 && scene.lights.size() == 1, "Rejected input changed retained resource sets");
    require(scene.textures.at(11).hash == 111 && scene.meshes.at(21).hash == 211, "Rejected input replaced a resource");
    require(scene.textures.at(11).pixels == before.contents, "Rejected input changed retained texture pixels");
    require(scene.textures.at(11).pixels.data() == before.pixels
        && scene.meshes.at(21).surfaces[0].vertices.data() == before.vertices, "Rejected input replaced retained allocations");
}
void rejects(SceneState& scene, const Bytes& bytes) {
    bool rejected = false;
    try { scene.apply(bytes); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "Malformed scene packet was accepted");
}
Bytes truncatedReset() {
    auto bytes = packet(SceneState::FullReset, 99, {});
    bytes[24] = std::byte{4}; // Claims a payload absent from the supplied span.
    return bytes;
}

void malformedResetIsAtomic() {
    auto scene = retainedScene(); const auto before = footprint(scene);
    rejects(scene, truncatedReset()); unchanged(scene, before);
}
void lateRecordTailIsAtomic() {
    auto scene = retainedScene(); const auto before = footprint(scene);
    auto badCamera = cameraRecord(9); u32(badCamera, 123);
    rejects(scene, packet(0, 18, {{1, textureRecord(11, 1, 90)}, {9, badCamera}}));
    unchanged(scene, before);
}
void lateFailureRollsBackPatchesAndRemoval() {
    auto scene = retainedScene(); const auto before = footprint(scene);
    rejects(scene, packet(0, 18, {{12, patchRecord(11, 1, 90)}, {6, idRecord(21)}, {99, {}}}));
    unchanged(scene, before);
}
void multiplePacketsAreOneTransaction() {
    auto scene = retainedScene(); const auto before = footprint(scene);
    auto bytes = packet(0, 18, {{2, idRecord(11)}, {9, cameraRecord(9)}});
    const auto malformed = truncatedReset(); bytes.insert(bytes.end(), malformed.begin(), malformed.end());
    rejects(scene, bytes); unchanged(scene, before);
}
void invalidMeshReplacementNeverCommits() {
    auto scene = retainedScene(); const auto before = footprint(scene);
    rejects(scene, packet(0, 18, {{5, meshRecord(21)}, {99, {}}}));
    unchanged(scene, before);
}
void cameraDeltaDoesNotCopyRetainedWorld() {
    auto scene = retainedScene(); const auto before = footprint(scene);
    scene.apply(packet(0, 18, {{9, cameraRecord(9)}}));
    require(scene.frameId == 18 && !scene.incomplete && scene.hasCamera && scene.camera.position[0] == 9,
        "Camera delta did not commit its metadata");
    require(scene.meshes.at(21).surfaces[0].vertices.data() == before.vertices
        && scene.textures.at(11).pixels.data() == before.pixels, "Camera delta copied the retained world");
}
void validResourceReplacementsCommit() {
    auto scene = retainedScene();
    scene.apply(packet(0, 18, {{1, textureRecord(11, 1, 40)}, {5, meshRecord(21)}}));
    require(scene.meshes.at(21).hash == 999 && scene.meshes.at(21).surfaces[0].vertices.size() == 3
        && scene.textures.at(11).width == 1, "Prepared resource nodes did not replace retained resources");
    require(scene.instances.at(31).mesh == 21 && scene.materials.at(12).textures[0] == 11,
        "Resource replacement altered retained references");
}
void texturePatchesCommitInPlace() {
    auto scene = retainedScene(); const auto before = footprint(scene);
    std::vector<Record> records{{12, patchRecord(11, 1, 90)}};
    for (unsigned i = 0; i < 64; ++i) records.push_back({9, cameraRecord(float(i))});
    records.push_back({12, patchRecord(11, 0, 80)});
    scene.apply(packet(0, 18, records));
    const auto& texture = scene.textures.at(11);
    require(texture.pixels.data() == before.pixels && texture.pixels[0] == std::byte{80}
        && texture.pixels[4] == std::byte{90}, "Texture patches copied the atlas or lost record ordering");
    require(scene.meshes.at(21).surfaces[0].vertices.data() == before.vertices, "Texture delta copied mesh storage");
}
void recreatedTextureUsesNewExtent() {
    auto scene = retainedScene();
    scene.apply(packet(0, 18, {{2, idRecord(11)}, {1, textureRecord(11, 1, 40)}, {12, patchRecord(11, 0, 80)}}));
    require(scene.textures.at(11).width == 1 && scene.textures.at(11).pixels.size() == 4
        && scene.textures.at(11).pixels[0] == std::byte{80}, "Patch did not target the recreated texture");
}
void emptyTexturePatchIsANoop() {
    auto scene = retainedScene(); const auto before = footprint(scene);
    Bytes empty;
    u64(empty, 11); u32(empty, 2); u32(empty, 0); u32(empty, 0); u32(empty, 1);
    u32(empty, 0); u32(empty, 0); u32(empty, 0);
    scene.apply(packet(0, 18, {{12, empty}}));
    require(scene.textures.at(11).pixels == before.contents && scene.textures.at(11).pixels.data() == before.pixels,
        "Zero-area texture patch changed retained pixels or allocation");
}
void removedOrResetTextureCannotBePatched() {
    auto scene = retainedScene(); const auto before = footprint(scene);
    rejects(scene, packet(0, 18, {{2, idRecord(11)}, {12, patchRecord(11, 0, 80)}}));
    unchanged(scene, before);
    rejects(scene, packet(SceneState::FullReset, 19, {{12, patchRecord(11, 0, 80)}}));
    unchanged(scene, before);
    rejects(scene, packet(0, 20, {{1, textureRecord(11, 1, 40)}, {12, patchRecord(11, 1, 80)}}));
    unchanged(scene, before);
}
void emptyFullResetClearsEveryResource() {
    auto scene = retainedScene();
    scene.apply(packet(SceneState::FullReset | SceneState::Incomplete, 20, {}));
    require(scene.frameId == 20 && scene.incomplete && !scene.hasCamera, "Empty reset metadata was not committed");
    require(scene.textures.empty() && scene.materials.empty() && scene.meshes.empty()
        && scene.instances.empty() && scene.lights.empty(), "Empty reset retained resources from the previous scene");
}
void emptyInputDoesNothing() {
    auto scene = retainedScene(); const auto before = footprint(scene);
    scene.apply({}); unchanged(scene, before);
}
} // namespace

int main() {
    try {
        malformedResetIsAtomic();
        lateRecordTailIsAtomic();
        lateFailureRollsBackPatchesAndRemoval();
        multiplePacketsAreOneTransaction();
        invalidMeshReplacementNeverCommits();
        cameraDeltaDoesNotCopyRetainedWorld();
        validResourceReplacementsCommit();
        texturePatchesCommitInPlace();
        recreatedTextureUsesNewExtent();
        emptyTexturePatchIsANoop();
        removedOrResetTextureCannotBePatched();
        emptyFullResetClearsEveryResource();
        emptyInputDoesNothing();
        std::cout << "ScenePacket transaction and retained-storage contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ScenePacket contract failed: " << error.what() << '\n';
        return 1;
    }
}
