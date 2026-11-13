/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 */
#include <renderer_lab/ScenePacket.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace renderer_lab {
namespace {
class Reader {
    std::span<const std::byte> data;
    std::size_t offset{};
public:
    explicit Reader(std::span<const std::byte> bytes) : data(bytes) {}
    std::size_t remaining() const { return data.size() - offset; }
    std::span<const std::byte> take(std::size_t n) {
        if (n > remaining()) throw std::runtime_error("Truncated RSLB record");
        auto result = data.subspan(offset, n);
        offset += n;
        return result;
    }
    std::uint32_t u32() {
        auto bytes = take(4);
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(bytes[i]) << (8 * i);
        return value;
    }
    std::uint64_t u64() { auto low = u32(); return low | (std::uint64_t(u32()) << 32); }
    float f32() {
        float result = std::bit_cast<float>(u32());
        if (!std::isfinite(result)) throw std::runtime_error("Non-finite RSLB float");
        return result;
    }
    template<std::size_t N> std::array<float, N> floats() {
        std::array<float, N> result{};
        for (auto& value : result) value = f32();
        return result;
    }
    std::string string() {
        auto bytes = take(u32());
        std::string result(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (result.find('\0') != std::string::npos) throw std::runtime_error("NUL in RSLB string");
        return result;
    }
    void finish() const { if (remaining()) throw std::runtime_error("Unexpected RSLB record tail"); }
};

std::size_t pixelBytes(std::uint32_t format) {
    switch (format) {
    case 0: case 1: case 2: case 3: case 8: return 4;
    case 4: return 1;
    case 5: return 2;
    case 6: return 8;
    case 7: return 16;
    default: throw std::runtime_error("Unsupported RSLB texture format " + std::to_string(format));
    }
}
std::size_t areaBytes(std::uint32_t width, std::uint32_t height, std::size_t stride) {
    if (width == 0 || height == 0 || width > 32768 || height > 32768)
        throw std::runtime_error("Invalid RSLB texture extent");
    return std::size_t(width) * height * stride;
}
Texture readTexture(Reader& reader) {
    Texture texture;
    texture.id = reader.u64(); texture.hash = reader.u64();
    texture.width = reader.u32(); texture.height = reader.u32();
    texture.format = reader.u32(); texture.purpose = reader.u32();
    auto bytes = reader.take(reader.u32());
    if (bytes.size() != areaBytes(texture.width, texture.height, pixelBytes(texture.format)))
        throw std::runtime_error("RSLB texture byte count does not match extent");
    texture.pixels.assign(bytes.begin(), bytes.end());
    return texture;
}
Material readMaterial(Reader& reader) {
    Material material;
    material.id = reader.u64(); material.hash = reader.u64();
    material.kind = reader.u32(); material.flags = reader.u32();
    for (auto& texture : material.textures) texture = reader.u64();
    material.baseColor = reader.floats<4>(); material.emissiveColor = reader.floats<3>();
    material.emissiveIntensity = reader.f32(); material.roughness = reader.f32();
    material.metallic = reader.f32(); material.ior = reader.f32(); material.alphaCutoff = reader.f32();
    material.alphaMode = reader.u32(); reader.u32();
    if (reader.remaining()) material.metadata = reader.string();
    if (material.kind > 1 || material.alphaMode > 2) throw std::runtime_error("Unknown RSLB material kind");
    return material;
}
Mesh readMesh(Reader& reader) {
    Mesh mesh;
    mesh.id = reader.u64(); mesh.hash = reader.u64();
    const auto count = reader.u32();
    if (count > reader.remaining() / 40) throw std::runtime_error("Invalid RSLB surface count");
    mesh.surfaces.reserve(count);
    for (std::uint32_t s = 0; s < count; ++s) {
        Surface surface;
        surface.material = reader.u64();
        auto vertexCount = reader.u32(), indexCount = reader.u32(), stride = reader.u32();
        surface.flags = reader.u32(); surface.bonesPerVertex = reader.u32();
        auto weights = reader.u32(), blendIndices = reader.u32(); reader.u32();
        if (stride != 64 || indexCount % 3 != 0 || vertexCount > reader.remaining() / 64)
            throw std::runtime_error("Invalid RSLB triangle vertex layout");
        surface.vertices.reserve(vertexCount);
        for (std::uint32_t v = 0; v < vertexCount; ++v) {
            Reader vertex(reader.take(64));
            Vertex value;
            value.position = vertex.floats<3>(); value.normal = vertex.floats<3>();
            value.uv = vertex.floats<2>(); value.argb = vertex.u32();
            value.texcoordBounds[0] = vertex.u32(); value.texcoordBounds[1] = vertex.u32();
            surface.vertices.push_back(value);
        }
        if (indexCount > reader.remaining() / 4) throw std::runtime_error("Truncated RSLB indices");
        surface.indices.reserve(indexCount);
        for (std::uint32_t i = 0; i < indexCount; ++i) {
            auto index = reader.u32();
            if (index >= vertexCount) throw std::runtime_error("RSLB index exceeds vertex count");
            surface.indices.push_back(index);
        }
        if (weights > reader.remaining() / 4) throw std::runtime_error("Truncated RSLB weights");
        for (std::uint32_t i = 0; i < weights; ++i) surface.weights.push_back(reader.f32());
        if (blendIndices > reader.remaining() / 4) throw std::runtime_error("Truncated RSLB blend indices");
        for (std::uint32_t i = 0; i < blendIndices; ++i) surface.blendIndices.push_back(reader.u32());
        if (surface.bonesPerVertex && (surface.weights.size() != std::size_t(vertexCount) * surface.bonesPerVertex
                || surface.blendIndices.size() != surface.weights.size()))
            throw std::runtime_error("RSLB skinning arrays do not match vertex count");
        mesh.surfaces.push_back(std::move(surface));
    }
    return mesh;
}
Instance readInstance(Reader& reader) {
    Instance instance;
    instance.id = reader.u64(); instance.mesh = reader.u64();
    instance.categoryFlags = reader.u32(); instance.doubleSided = reader.u32() != 0;
    instance.transform = reader.floats<12>();
    if (reader.remaining()) {
        auto bones = reader.u32(), transforms = reader.u32(), jsonBytes = reader.u32(); reader.u32();
        if (std::uint64_t(bones + std::uint64_t(transforms)) * 48 + jsonBytes > reader.remaining())
            throw std::runtime_error("Truncated RSLB instance extensions");
        for (std::uint32_t i = 0; i < bones; ++i) instance.bones.push_back(reader.floats<12>());
        for (std::uint32_t i = 0; i < transforms; ++i) instance.gpuTransforms.push_back(reader.floats<12>());
        auto bytes = reader.take(jsonBytes);
        instance.blendMetadata.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    return instance;
}
Camera readCamera(Reader& reader) {
    Camera camera;
    camera.type = reader.u32(); camera.flags = reader.u32();
    camera.width = reader.u32(); camera.height = reader.u32();
    camera.view = reader.floats<16>(); camera.projection = reader.floats<16>();
    camera.position = reader.floats<3>(); camera.forward = reader.floats<3>();
    camera.up = reader.floats<3>(); camera.right = reader.floats<3>();
    camera.fovYDegrees = reader.f32(); camera.aspect = reader.f32();
    camera.zNear = reader.f32(); camera.zFar = reader.f32();
    if (!camera.width || !camera.height || camera.fovYDegrees <= 0 || camera.fovYDegrees >= 180
            || camera.aspect <= 0 || camera.zNear <= 0)
        throw std::runtime_error("Invalid RSLB camera projection");
    return camera;
}
Light readLight(Reader& reader) {
    Light light;
    light.id = reader.u64(); light.hash = reader.u64(); light.type = reader.u32(); light.flags = reader.u32();
    light.radiance = reader.floats<3>(); light.position = reader.floats<3>();
    light.direction = reader.floats<3>(); light.xAxis = reader.floats<3>(); light.yAxis = reader.floats<3>();
    light.size = reader.floats<4>(); light.colorTexture = reader.u64(); light.transform = reader.floats<12>();
    if (reader.remaining()) light.metadata = reader.string();
    if (light.type > 5) throw std::runtime_error("Unknown RSLB light type");
    return light;
}
struct TexturePatch {
    Id id{};
    std::uint32_t x{}, y{}, width{}, height{}, pitch{}, pixelSize{};
    std::vector<std::byte> pixels;
};
TexturePatch readTexturePatch(Id id, const Texture& texture, Reader& reader) {
    auto x = reader.u32(), y = reader.u32(), width = reader.u32(), height = reader.u32();
    auto pitch = reader.u32(), format = reader.u32(), count = reader.u32();
    auto bytes = reader.take(count);
    if (texture.pixels.empty()) throw std::runtime_error("Patching a file-backed texture requires image decoding and is not supported by this replay prototype");
    auto bpp = pixelBytes(format);
    if (texture.format != format || x > texture.width || y > texture.height
            || width > texture.width - x || height > texture.height - y || pitch < std::size_t(width) * bpp
            || (height && std::uint64_t(height - 1) * pitch + width * bpp > count))
        throw std::runtime_error("Invalid RSLB texture patch extent");
    TexturePatch patch{id, x, y, width, height, pitch, static_cast<std::uint32_t>(bpp), {}};
    patch.pixels.assign(bytes.begin(), bytes.end());
    return patch;
}

template<class T> using ResourceMap = std::map<Id, T>;
template<class T> typename ResourceMap<T>::node_type stageResource(T value) {
    ResourceMap<T> staging;
    auto entry = staging.emplace(value.id, std::move(value)).first;
    return staging.extract(entry);
}
using TextureNode = ResourceMap<Texture>::node_type;
using MaterialNode = ResourceMap<Material>::node_type;
using MeshNode = ResourceMap<Mesh>::node_type;
using InstanceNode = ResourceMap<Instance>::node_type;
using LightNode = ResourceMap<Light>::node_type;
struct FrameAction { std::uint32_t flags; std::uint64_t frame; };
struct EraseAction { std::uint32_t operation; Id id; };
using Action = std::variant<FrameAction, EraseAction, TextureNode, MaterialNode, MeshNode, InstanceNode, Camera, LightNode, TexturePatch>;

template<class T> void commitResource(ResourceMap<T>& destination, typename ResourceMap<T>::node_type& node) {
    destination.erase(node.key());
    // Both maps use the same default allocator. Moving a prepared node allocates
    // nothing; scalar-key comparison and resource destruction cannot throw.
    destination.insert(std::move(node));
}
} // namespace

void SceneState::apply(std::span<const std::byte> packets) {
    std::vector<Action> actions;
    std::map<Id, const Texture*> stagedTextures;
    bool texturesReset = false;
    auto stageTexture = [&](Texture value) {
        auto& action = actions.emplace_back(stageResource(std::move(value)));
        auto& texture = std::get<TextureNode>(action).mapped();
        // A map node's allocation remains stable when its handle/action moves.
        stagedTextures[texture.id] = &texture;
    };
    auto projectedTexture = [&](Id id) -> const Texture& {
        auto pending = stagedTextures.find(id);
        if (pending != stagedTextures.end()) {
            if (!pending->second) throw std::runtime_error("Patch references an unknown texture");
            return *pending->second;
        }
        auto retained = textures.find(id);
        if (texturesReset || retained == textures.end()) throw std::runtime_error("Patch references an unknown texture");
        return retained->second;
    };
    Reader stream(packets);
    while (stream.remaining()) {
        if (stream.u32() != Magic || stream.u32() != Version) throw std::runtime_error("Unsupported RSLB packet version");
        auto flags = stream.u32(), records = stream.u32();
        auto nextFrame = stream.u64(); auto payloadBytes = stream.u32(); stream.u32();
        if (flags & ~(FullReset | Incomplete)) throw std::runtime_error("Unknown RSLB packet flags");
        Reader payload(stream.take(payloadBytes));
        if (records > payload.remaining() / 8) throw std::runtime_error("Invalid RSLB record count");
        actions.emplace_back(FrameAction{flags, nextFrame});
        if (flags & FullReset) {
            stagedTextures.clear();
            texturesReset = true;
        }
        for (std::uint32_t n = 0; n < records; ++n) {
            auto operation = payload.u32(); Reader record(payload.take(payload.u32()));
            switch (operation) {
            case 1: stageTexture(readTexture(record)); break;
            case 2: {
                auto id = record.u64();
                actions.emplace_back(EraseAction{operation, id}); stagedTextures[id] = nullptr; break;
            }
            case 3: actions.emplace_back(stageResource(readMaterial(record))); break;
            case 4: case 6: case 8: case 11: actions.emplace_back(EraseAction{operation, record.u64()}); break;
            case 5: actions.emplace_back(stageResource(readMesh(record))); break;
            case 7: actions.emplace_back(stageResource(readInstance(record))); break;
            case 9: actions.emplace_back(readCamera(record)); break;
            case 10: actions.emplace_back(stageResource(readLight(record))); break;
            case 12: {
                auto id = record.u64();
                actions.emplace_back(readTexturePatch(id, projectedTexture(id), record)); break;
            }
            case 13: {
                Texture value; value.id = record.u64(); value.file = std::filesystem::u8path(record.string());
                stageTexture(std::move(value)); break;
            }
            default: throw std::runtime_error("Unsupported RSLB operation " + std::to_string(operation));
            }
            record.finish();
        }
        payload.finish();
    }
    // Decode the entire stream before touching retained state. Prepared map
    // nodes make the commit allocation-free, including replacements and resets.
    for (auto& action : actions) {
        std::visit([&](auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, FrameAction>) {
                if (value.flags & FullReset) *this = SceneState{};
                incomplete = (value.flags & Incomplete) != 0;
                frameId = value.frame;
            } else if constexpr (std::is_same_v<T, EraseAction>) {
                switch (value.operation) {
                case 2: textures.erase(value.id); break;
                case 4: materials.erase(value.id); break;
                case 6: meshes.erase(value.id); break;
                case 8: instances.erase(value.id); break;
                case 11: lights.erase(value.id); break;
                }
            } else if constexpr (std::is_same_v<T, TextureNode>) commitResource(textures, value);
            else if constexpr (std::is_same_v<T, MaterialNode>) commitResource(materials, value);
            else if constexpr (std::is_same_v<T, MeshNode>) commitResource(meshes, value);
            else if constexpr (std::is_same_v<T, InstanceNode>) commitResource(instances, value);
            else if constexpr (std::is_same_v<T, LightNode>) commitResource(lights, value);
            else if constexpr (std::is_same_v<T, Camera>) { camera = value; hasCamera = true; }
            else if constexpr (std::is_same_v<T, TexturePatch>) {
                // Projection validated existence/format/extents in record order.
                // Retain only changed rows, never copy the resident atlas.
                if (!value.width || !value.height) return;
                auto& texture = textures.find(value.id)->second;
                const auto bpp = value.pixelSize;
                for (std::uint32_t row = 0; row < value.height; ++row)
                    std::memcpy(texture.pixels.data() + ((value.y + row) * std::size_t(texture.width) + value.x) * bpp,
                        value.pixels.data() + row * std::size_t(value.pitch), value.width * bpp);
            }
        }, action);
    }
}

void SceneState::load(const std::filesystem::path& packetFile) {
    std::ifstream file(packetFile, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open scene packet: " + packetFile.string());
    auto size = file.tellg();
    if (size < 32 || std::uint64_t(size) > std::uint64_t(8) * 1024 * 1024 * 1024)
        throw std::runtime_error("Invalid scene packet file size");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!file) throw std::runtime_error("Could not read complete scene packet");
    apply(bytes);
}

void SceneState::validate() const {
    if (!hasCamera) throw std::runtime_error("Scene packet has no camera");
    if (instances.empty()) throw std::runtime_error("Scene packet has no mesh instances");
    for (const auto& [id, instance] : instances) {
        auto mesh = meshes.find(instance.mesh);
        if (mesh == meshes.end()) throw std::runtime_error("Instance references absent mesh " + std::to_string(instance.mesh));
        for (const auto& surface : mesh->second.surfaces) {
            if (!materials.contains(surface.material)) throw std::runtime_error("Surface references absent material " + std::to_string(surface.material));
            for (auto texture : materials.at(surface.material).textures)
                if (texture && !textures.contains(texture)) throw std::runtime_error("Material references absent texture " + std::to_string(texture));
        }
    }
}
} // namespace renderer_lab
