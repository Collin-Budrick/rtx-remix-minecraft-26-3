/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 */
#include <renderer_lab/GltfExport.h>
#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace renderer_lab {
namespace {
template<class T> std::string arrayJson(const T& v) {
    std::ostringstream s; s << std::setprecision(9) << '[';
    bool comma = false; for (auto x : v) { if (comma) s << ','; s << x; comma = true; }
    return s.str() + ']';
}
std::string join(const std::vector<std::string>& v) {
    std::string s; for (const auto& x : v) { if (!s.empty()) s += ','; s += x; } return s;
}
void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary); f.write(text.data(), std::streamsize(text.size()));
    if (!f) throw std::runtime_error("Cannot write " + path.string());
}
float halfFloat(std::uint16_t h) {
    int exponent = (h >> 10) & 31; auto mantissa = h & 1023;
    float f = exponent == 0 ? std::ldexp(float(mantissa), -24)
        : exponent == 31 ? std::numeric_limits<float>::infinity()
        : std::ldexp(float(mantissa + 1024), exponent - 25);
    return h & 0x8000 ? -f : f;
}
std::array<float,4> pixel(const Texture& t, std::size_t i) {
    const auto* p = reinterpret_cast<const unsigned char*>(t.pixels.data());
    auto byte = [&](std::size_t at) { return float(p[at]) / 255.f; };
    switch (t.format) {
    case 0: case 1: return {byte(4*i),byte(4*i+1),byte(4*i+2),byte(4*i+3)};
    case 2: case 3: return {byte(4*i+2),byte(4*i+1),byte(4*i),byte(4*i+3)};
    case 4: return {byte(i),byte(i),byte(i),1};
    case 5: return {byte(2*i),byte(2*i+1),0,1};
    case 6: {
        std::array<std::uint16_t,4> h; std::memcpy(h.data(), p+i*8, 8);
        return {halfFloat(h[0]),halfFloat(h[1]),halfFloat(h[2]),halfFloat(h[3])};
    }
    case 7: { std::array<float,4> f; std::memcpy(f.data(), p+i*16,16); return f; }
    case 8: { float f; std::memcpy(&f,p+i*4,4); return {f,f,f,1}; }
    default: throw std::runtime_error("Unsupported image format");
    }
}
void writePng(const std::filesystem::path& path, std::uint32_t w, std::uint32_t h,
              const std::vector<unsigned char>& rgba) {
#ifdef _WIN32
    using Microsoft::WRL::ComPtr;
    auto check = [](HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("WIC PNG encoding failed: " + std::to_string(unsigned(hr))); };
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComExit { bool owned; ~ComExit() { if (owned) CoUninitialize(); } } exit{SUCCEEDED(init)};
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) check(init);
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream; check(factory->CreateStream(&stream));
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder; check(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame; check(encoder->CreateNewFrame(&frame,nullptr));
    check(frame->Initialize(nullptr)); check(frame->SetSize(w,h));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat32bppBGRA) throw std::runtime_error("WIC cannot encode BGRA PNG");
    if (rgba.size() > UINT_MAX) throw std::runtime_error("PNG is too large");
    auto bgra = rgba;
    for (std::size_t offset=0;offset<bgra.size();offset+=4) std::swap(bgra[offset],bgra[offset+2]);
    check(frame->WritePixels(h,w*4,UINT(bgra.size()),bgra.data()));
    check(frame->Commit()); check(encoder->Commit());
#else
    throw std::runtime_error("PNG export requires Windows WIC");
#endif
}
void writeHdr(const std::filesystem::path& path, const Texture& t) {
    std::ofstream f(path,std::ios::binary);
    f << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " << t.height << " +X " << t.width << '\n';
    // Flat RGBE is valid Radiance data; Donut's stb decoder accepts it.
    for (std::size_t i=0;i<std::size_t(t.width)*t.height;++i) {
        auto c = pixel(t,i); float m = std::max({0.f,c[0],c[1],c[2]}); unsigned char out[4]{};
        if (std::isfinite(m) && m > 1e-32f) {
            int e; float scale = std::frexp(m,&e)*256.f/m;
            for (int k=0;k<3;++k) out[k] = static_cast<unsigned char>(std::clamp(c[k]*scale,0.f,255.f));
            out[3] = static_cast<unsigned char>(std::clamp(e+128,0,255));
        }
        f.write(reinterpret_cast<char*>(out),4);
    }
    if (!f) throw std::runtime_error("Cannot write HDR " + path.string());
}
std::vector<unsigned char> rgba8(const Texture& t) {
    std::vector<unsigned char> out(std::size_t(t.width)*t.height*4);
    for (std::size_t i=0;i<out.size()/4;++i) {
        auto c = pixel(t,i);
        for (int k=0;k<4;++k) out[i*4+k] = static_cast<unsigned char>(std::clamp(c[k],0.f,1.f)*255.f+.5f);
    }
    return out;
}
using V3 = std::array<float,3>;
V3 transformPoint(const Matrix34& m, const V3& p, bool direction=false) {
    V3 v{}; for (int r=0;r<3;++r) v[r]=m[4*r]*p[0]+m[4*r+1]*p[1]+m[4*r+2]*p[2]+(direction?0.f:m[4*r+3]); return v;
}
V3 normalized(V3 v) {
    float len=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
    if (len<1e-12f) throw std::runtime_error("Invalid zero direction in scene");
    for (auto& x:v) x/=len; return v;
}
std::array<float,4> rotationFromDirection(V3 d) {
    d=normalized(d); // quaternion rotating local -Z to the requested light direction
    float dot=-d[2];
    if (dot < -.999999f) return {0,1,0,0};
    float s=std::sqrt((1.f+dot)*2.f);
    return {d[1]/s,-d[0]/s,0,s*.5f};
}
std::array<float,16> matrix(const Matrix34& m) {
    return {m[0],m[4],m[8],0,m[1],m[5],m[9],0,m[2],m[6],m[10],0,m[3],m[7],m[11],1};
}
struct Binary {
    std::vector<std::byte> data;
    std::vector<std::string> views,accessors;
    template<class T> std::size_t append(const std::vector<T>& v, int target) {
        while (data.size()%4) data.push_back(std::byte{});
        auto offset=data.size(); const auto* p=reinterpret_cast<const std::byte*>(v.data());
        data.insert(data.end(),p,p+v.size()*sizeof(T));
        auto id=views.size(); views.push_back("{\"buffer\":0,\"byteOffset\":"+std::to_string(offset)+",\"byteLength\":"+std::to_string(v.size()*sizeof(T))+",\"target\":"+std::to_string(target)+"}"); return id;
    }
    template<class T> std::size_t accessor(const std::vector<T>& v, int component, const char* type, int target, std::string extra={}) {
        auto view=append(v,target); auto id=accessors.size();
        accessors.push_back("{\"bufferView\":"+std::to_string(view)+",\"componentType\":"+std::to_string(component)+",\"count\":"+std::to_string(v.size())+",\"type\":"+jsonString(type)+extra+"}"); return id;
    }
};
} // namespace

std::string jsonString(const std::string& value) {
    std::ostringstream s; s << '"';
    for (unsigned char c:value) {
        if (c=='"'||c=='\\') s << '\\' << char(c);
        else if (c<32) s << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else s << char(c);
    }
    return s.str()+'"';
}

ExportResult exportGltf(const SceneState& scene, const std::filesystem::path& directory, const ExportOptions& options) {
    scene.validate();
    ExportResult result;
    auto dir=std::filesystem::absolute(directory); std::filesystem::create_directories(dir/"textures");
    auto limitation=[&](std::string text) {
        if (std::find(result.limitations.begin(),result.limitations.end(),text)!=result.limitations.end()) return;
        result.limitations.push_back(text);
        if (!options.allowApproximations) throw std::runtime_error(text+"; explicitly use --allow-approximations to export with a warning manifest");
    };
    if (scene.incomplete) limitation("Java capture marked this packet incomplete: opaque extension data was not representable");
    // InstanceCategoryBit.HIDDEN and IGNORE retain resources in the Remix API
    // without drawing them (e.g. terrain pages awaiting replacement/LOD clips).
    constexpr std::uint32_t hiddenOrIgnored=(1u<<8)|(1u<<3);
    std::set<Id> usedMaterials,usedTextures;
    for (const auto& [id,instance]:scene.instances) {
        if (instance.categoryFlags&hiddenOrIgnored) continue;
        for (const auto& surface:scene.meshes.at(instance.mesh).surfaces) usedMaterials.insert(surface.material);
    }
    for (auto id:usedMaterials)
        for (auto texture:scene.materials.at(id).textures) if (texture) usedTextures.insert(texture);
    for (const auto& [id,light]:scene.lights) if (light.colorTexture) usedTextures.insert(light.colorTexture);
    std::map<Id,std::string> imageFiles;
    std::map<Id,std::size_t> textureIndices;
    std::vector<std::string> images,textures,materials,meshes,nodes,graph;
    for (const auto& [id,t]:scene.textures) {
        if (!usedTextures.contains(id)) continue;
        std::string name="textures/"+std::to_string(id);
        if (!t.file.empty()) {
            if (!std::filesystem::is_regular_file(t.file)) throw std::runtime_error("Captured texture file does not exist: "+t.file.string());
            name+=t.file.extension().string(); std::filesystem::copy_file(t.file,dir/name,std::filesystem::copy_options::overwrite_existing);
        } else {
            bool hdr=t.format>=6; name+=hdr?".hdr":".png";
            if (hdr) writeHdr(dir/name,t); else writePng(dir/name,t.width,t.height,rgba8(t));
        }
        imageFiles[id]=name; textureIndices[id]=textures.size();
        images.push_back("{\"uri\":"+jsonString(name)+"}");
        textures.push_back("{\"sampler\":0,\"source\":"+std::to_string(images.size()-1)+"}");
    }
    std::map<Id,std::size_t> packedRoughMetal;
    for (const auto& [id,m]:scene.materials) {
        if (!usedMaterials.contains(id)) continue;
        if (m.textures[5]||m.textures[7]) limitation("Height and tangent textures are not used by the glTF renderer (material "+std::to_string(id)+")");
        if (m.kind==1) limitation("Remix translucent/refraction semantics are approximated with glTF transmission (material "+std::to_string(id)+")");
        if (m.textures[6]) limitation("Textured transmittance is reduced to scalar transmission (material "+std::to_string(id)+")");
        Id rough=m.textures[3], metal=m.textures[4];
        if (!rough&&!metal) continue;
        const Texture& base=scene.textures.at(rough?rough:metal);
        if (!base.file.empty() || (rough&&!scene.textures.at(rough).file.empty()) || (metal&&!scene.textures.at(metal).file.empty())) {
            limitation("File-backed roughness/metallic maps need offline channel packing (material "+std::to_string(id)+")"); continue;
        }
        if ((rough&&(scene.textures.at(rough).width!=base.width||scene.textures.at(rough).height!=base.height)) ||
            (metal&&(scene.textures.at(metal).width!=base.width||scene.textures.at(metal).height!=base.height))) {
            limitation("Roughness/metallic texture dimensions differ (material "+std::to_string(id)+")"); continue;
        }
        std::vector<unsigned char> pixels(std::size_t(base.width)*base.height*4,255);
        for (std::size_t i=0;i<pixels.size()/4;++i) {
            pixels[4*i+1]=rough?static_cast<unsigned char>(std::clamp(pixel(scene.textures.at(rough),i)[0],0.f,1.f)*255.f+.5f):255;
            pixels[4*i+2]=metal?static_cast<unsigned char>(std::clamp(pixel(scene.textures.at(metal),i)[0],0.f,1.f)*255.f+.5f):255;
        }
        std::string name="textures/mr_"+std::to_string(id)+".png"; writePng(dir/name,base.width,base.height,pixels);
        packedRoughMetal[id]=textures.size(); images.push_back("{\"uri\":"+jsonString(name)+"}");
        textures.push_back("{\"sampler\":0,\"source\":"+std::to_string(images.size()-1)+"}");
    }
    struct BlendState { int alphaMode=-1; float cutoff=.5f; };
    auto instanceBlend=[&](const Instance& instance) {
        BlendState state;
        if (instance.blendMetadata.empty()) return state;
        auto j=nlohmann::json::parse(instance.blendMetadata);
        if (!j.is_object()) throw std::runtime_error("Instance blend metadata must be a JSON object");
        const std::set<std::string> known{"alphaTestEnabled","alphaTestReferenceValue","alphaTestCompareOp","alphaBlendEnabled","srcColorBlendFactor","dstColorBlendFactor","colorBlendOp","srcAlphaBlendFactor","dstAlphaBlendFactor","alphaBlendOp","textureColorArg1Source","textureColorArg2Source","textureColorOperation","textureAlphaArg1Source","textureAlphaArg2Source","textureAlphaOperation","tFactor","isTextureFactorBlend","writeMask","isVertexColorBakedLighting"};
        for (auto it=j.begin();it!=j.end();++it) if (!known.contains(it.key()))
            limitation("Unknown instance blend field "+it.key()+" (instance "+std::to_string(instance.id)+")");
        auto integer=[&](const char* key,int fallback) { return j.value(key,fallback); };
        bool standardColor=integer("textureColorArg1Source",1)==1 && integer("textureColorArg2Source",2)==2 && integer("textureColorOperation",3)==3;
        bool standardAlpha=integer("textureAlphaArg1Source",1)==1 &&
            ((integer("textureAlphaArg2Source",2)==2 && integer("textureAlphaOperation",3)==3) ||
             (integer("textureAlphaArg2Source",0)==0 && integer("textureAlphaOperation",1)==1));
        if (!standardColor||!standardAlpha||integer("isTextureFactorBlend",0)||integer("writeMask",15)!=15||integer("isVertexColorBakedLighting",0))
            limitation("Nonstandard texture/tint/write-mask blend operations are not reproduced (instance "+std::to_string(instance.id)+")");
        if (integer("alphaBlendEnabled",0)) {
            bool standard=integer("srcColorBlendFactor",6)==6 && integer("dstColorBlendFactor",7)==7 && integer("colorBlendOp",0)==0 &&
                integer("srcAlphaBlendFactor",1)==1 && integer("dstAlphaBlendFactor",7)==7 && integer("alphaBlendOp",0)==0;
            if (!standard) limitation("Nonstandard alpha blending is reduced to glTF BLEND (instance "+std::to_string(instance.id)+")");
            if (integer("alphaTestEnabled",0)) limitation("Simultaneous alpha test and alpha blend is reduced to glTF BLEND (instance "+std::to_string(instance.id)+")");
            state.alphaMode=2;
        } else if (integer("alphaTestEnabled",0)) {
            int compare=integer("alphaTestCompareOp",7),reference=integer("alphaTestReferenceValue",0);
            if (reference<0||reference>255) throw std::runtime_error("Alpha test reference must be unsigned byte");
            if (compare==4||compare==6) {
                state.alphaMode=1; state.cutoff=float(reference)/255.f;
                if (compare==4) state.cutoff=std::nextafter(state.cutoff,INFINITY);
            } else if (compare==7) state.alphaMode=0;
            else limitation("Alpha test compare operation cannot be represented by glTF MASK (instance "+std::to_string(instance.id)+")");
        } else state.alphaMode=0;
        return state;
    };
    using MaterialKey=std::tuple<Id,std::uint32_t,bool,int,float>;
    std::map<MaterialKey,std::size_t> variants;
    auto material=[&](Id id,std::uint32_t tint,bool doubleSided,const BlendState& blend) {
        MaterialKey key{id,tint,doubleSided,blend.alphaMode,blend.cutoff}; if (variants.contains(key)) return variants.at(key);
        const auto& m=scene.materials.at(id); auto c=m.baseColor;
        int alphaMode=blend.alphaMode<0?int(m.alphaMode):blend.alphaMode;
        float cutoff=blend.alphaMode==1?blend.cutoff:m.alphaCutoff;
        c[0]*=float((tint>>16)&255)/255.f; c[1]*=float((tint>>8)&255)/255.f; c[2]*=float(tint&255)/255.f; c[3]*=float(tint>>24)/255.f;
        auto texInfo=[&](Id tid) { return "{\"index\":"+std::to_string(textureIndices.at(tid))+"}"; };
        std::ostringstream s; s << std::setprecision(9) << "{\"name\":\"material_" << id << "_" << tint << "\",\"doubleSided\":" << (doubleSided?"true":"false")
            << ",\"alphaMode\":" << jsonString(alphaMode==1?"MASK":alphaMode==2?"BLEND":"OPAQUE") << ",\"alphaCutoff\":" << cutoff
            << ",\"pbrMetallicRoughness\":{\"baseColorFactor\":" << arrayJson(c) << ",\"roughnessFactor\":" << m.roughness << ",\"metallicFactor\":" << m.metallic;
        if (m.textures[0]) s << ",\"baseColorTexture\":" << texInfo(m.textures[0]);
        if (packedRoughMetal.contains(id)) s << ",\"metallicRoughnessTexture\":{\"index\":" << packedRoughMetal.at(id) << '}';
        s << '}'; if (m.textures[1]) s << ",\"normalTexture\":" << texInfo(m.textures[1]);
        if (m.textures[2]) s << ",\"emissiveTexture\":" << texInfo(m.textures[2]);
        auto emission=m.emissiveColor; float emissionScale=std::max({1.f,emission[0],emission[1],emission[2]});
        for (auto& channel:emission) channel/=emissionScale;
        s << ",\"emissiveFactor\":" << arrayJson(emission) << ",\"extensions\":{\"KHR_materials_emissive_strength\":{\"emissiveStrength\":" << m.emissiveIntensity*emissionScale << "},\"KHR_materials_ior\":{\"ior\":" << m.ior << '}';
        if (m.kind==1) s << ",\"KHR_materials_transmission\":{\"transmissionFactor\":1}";
        s << "},\"extras\":{\"remixId\":" << jsonString(std::to_string(id)) << ",\"sourceMetadata\":" << jsonString(m.metadata) << "}}";
        auto index=materials.size(); materials.push_back(s.str()); variants[key]=index; return index;
    };
    Binary binary;
    // Reuse static mesh geometry between instances. Posed skinning is exported per instance.
    std::map<std::tuple<Id,bool,std::string>,std::size_t> meshCache;
    for (const auto& [id,instance]:scene.instances) {
        if (instance.categoryFlags&hiddenOrIgnored) continue;
        constexpr std::uint32_t ordinarySnapshotFlags=(1u<<5)|(1u<<6)|(1u<<7)|(1u<<16)|(1u<<20);
        auto specialCategories=instance.categoryFlags&~ordinarySnapshotFlags;
        if (specialCategories) limitation("Remix ray-visibility/decal/sky category behavior is not reproduced (mask "+std::to_string(specialCategories)+")");
        if (!instance.gpuTransforms.empty()) limitation("GPU instance-transform extension is not decoded (instance "+std::to_string(id)+")");
        auto blend=instanceBlend(instance);
        auto cacheKey=std::tuple{instance.mesh,instance.doubleSided,instance.blendMetadata}; std::size_t meshIndex;
        if (instance.bones.empty()&&meshCache.contains(cacheKey)) meshIndex=meshCache.at(cacheKey);
        else {
            std::vector<std::string> primitives;
            for (const auto& surface:scene.meshes.at(instance.mesh).surfaces) {
                auto vertices=surface.vertices;
                if (surface.bonesPerVertex) {
                    if (instance.bones.empty()) limitation("Skinned mesh has no captured bone matrices (instance "+std::to_string(id)+")");
                    else for (std::size_t vi=0;vi<vertices.size();++vi) {
                        V3 p{},n{}; float total=0;
                        for (std::uint32_t k=0;k<surface.bonesPerVertex;++k) {
                            auto at=vi*surface.bonesPerVertex+k; auto bone=surface.blendIndices.at(at); float w=surface.weights.at(at);
                            if (bone>=instance.bones.size()) throw std::runtime_error("Skin bone index exceeds captured pose");
                            auto bp=transformPoint(instance.bones[bone],vertices[vi].position); auto bn=transformPoint(instance.bones[bone],vertices[vi].normal,true);
                            for (int j=0;j<3;++j) { p[j]+=w*bp[j]; n[j]+=w*bn[j]; } total+=w;
                        }
                        if (total<1e-6f) throw std::runtime_error("Skin has zero total weight");
                        for (auto& x:p) x/=total; vertices[vi].position=p; vertices[vi].normal=normalized(n);
                    }
                }
                std::vector<V3> positions,normals; std::vector<std::array<float,2>> uv;
                V3 lo{INFINITY,INFINITY,INFINITY},hi{-INFINITY,-INFINITY,-INFINITY};
                for (const auto& v:vertices) {
                    positions.push_back(v.position); normals.push_back(v.normal); uv.push_back(v.uv);
                    for (int j=0;j<3;++j) { lo[j]=std::min(lo[j],v.position[j]); hi[j]=std::max(hi[j],v.position[j]); }
                }
                if (positions.empty()||surface.indices.empty()) continue;
                auto pos=binary.accessor(positions,5126,"VEC3",34962,",\"min\":"+arrayJson(lo)+",\"max\":"+arrayJson(hi));
                auto normal=binary.accessor(normals,5126,"VEC3",34962); auto texcoord=binary.accessor(uv,5126,"VEC2",34962);
                std::map<std::uint32_t,std::vector<std::uint32_t>> groups;
                for (std::size_t i=0;i<surface.indices.size();i+=3) {
                    auto tint=vertices[surface.indices[i]].argb;
                    if (vertices[surface.indices[i+1]].argb!=tint || vertices[surface.indices[i+2]].argb!=tint)
                        limitation("Nonuniform triangle vertex tint is reduced to its first vertex (mesh "+std::to_string(instance.mesh)+")");
                    auto& g=groups[tint]; g.insert(g.end(),surface.indices.begin()+i,surface.indices.begin()+i+3);
                }
                for (const auto& [tint,indices]:groups) {
                    auto index=binary.accessor(indices,5125,"SCALAR",34963);
                    primitives.push_back("{\"attributes\":{\"POSITION\":"+std::to_string(pos)+",\"NORMAL\":"+std::to_string(normal)+",\"TEXCOORD_0\":"+std::to_string(texcoord)+"},\"indices\":"+std::to_string(index)+",\"material\":"+std::to_string(material(surface.material,tint,instance.doubleSided,blend))+",\"mode\":4}");
                }
                result.vertices+=positions.size();
            }
            meshIndex=meshes.size(); meshes.push_back("{\"primitives\":["+join(primitives)+"]}");
            if (instance.bones.empty()) meshCache[cacheKey]=meshIndex;
        }
        nodes.push_back("{\"name\":\"instance_"+std::to_string(id)+"\",\"mesh\":"+std::to_string(meshIndex)+",\"matrix\":"+arrayJson(matrix(instance.transform))+",\"extras\":{\"remixCategoryFlags\":"+std::to_string(instance.categoryFlags)+"}}");
        for (const auto& s:scene.meshes.at(instance.mesh).surfaces) result.triangles+=s.indices.size()/3;
        ++result.instances;
    }
    if (!result.instances) throw std::runtime_error("The captured scene has no visible mesh instances after HIDDEN/IGNORE filtering");
    graph.push_back("{\"name\":\"Minecraft geometry\",\"model\":0}");
    for (const auto& [id,light]:scene.lights) {
        std::ostringstream s; s << std::setprecision(9) << "{\"name\":\"light_" << id << '"';
        if (light.type==5) {
            if (!light.colorTexture||!imageFiles.contains(light.colorTexture)) { limitation("Dome light has no captured texture (light "+std::to_string(id)+")"); continue; }
            if (light.size[0]!=1 || light.size[1]!=0) limitation("Dome blending is not reproduced (light "+std::to_string(id)+")");
            if (light.radiance!=V3{1,1,1}) limitation("Dome radiance tint is not reproduced (light "+std::to_string(id)+")");
            s << ",\"type\":\"EnvironmentLight\",\"intensity\":1,\"path\":" << jsonString((dir/imageFiles.at(light.colorTexture)).generic_string());
        } else if (light.type==1) {
            // Remix normalizes the distant cone by sin^2(half-angle), giving
            // pi*radiance irradiance (also documented by LightManager.SUN_DISC_SHARE).
            s << ",\"type\":\"DirectionalLight\",\"color\":" << arrayJson(light.radiance) << ",\"irradiance\":" << 3.14159265358979323846f
                << ",\"angularSize\":" << light.size[0] << ",\"rotation\":" << arrayJson(rotationFromDirection(light.direction));
        } else if (light.type==0) {
            float intensity=3.14159265358979323846f*light.size[0]*light.size[0];
            s << ",\"type\":\"PointLight\",\"color\":" << arrayJson(light.radiance) << ",\"intensity\":" << intensity << ",\"radius\":" << light.size[0]
                << ",\"translation\":" << arrayJson(light.position);
        } else { limitation("Rect/disk/cylinder analytical lights are omitted (light "+std::to_string(id)+")"); continue; }
        if (!light.metadata.empty()&&light.metadata!="{}") limitation("Extended light shaping/volumetric metadata is preserved in the manifest but not applied (light "+std::to_string(id)+")");
        s << '}'; graph.push_back(s.str());
    }
    const auto& camera=scene.camera;
    graph.push_back("{\"name\":\"Minecraft camera\",\"type\":\"PerspectiveCamera\",\"verticalFov\":"+std::to_string(camera.fovYDegrees*3.14159265358979323846/180.)+",\"aspectRatio\":"+std::to_string(camera.aspect)+",\"zNear\":"+std::to_string(camera.zNear)+",\"zFar\":"+std::to_string(camera.zFar)+",\"translation\":"+arrayJson(camera.position)+",\"rotation\":"+arrayJson(rotationFromDirection(camera.forward))+"}");
    std::vector<std::string> nodeIndices; for (std::size_t i=0;i<nodes.size();++i) nodeIndices.push_back(std::to_string(i));
    std::ofstream bin(dir/"world.bin",std::ios::binary); bin.write(reinterpret_cast<const char*>(binary.data.data()),std::streamsize(binary.data.size()));
    if (!bin) throw std::runtime_error("Cannot write world.bin"); bin.close();
    result.gltfFile=dir/"world.gltf"; result.sceneFile=dir/"world.scene.json";
    writeText(result.gltfFile,"{\"asset\":{\"version\":\"2.0\",\"generator\":\"Minecraft renderer lab RSLB v1\"},\"extensionsUsed\":[\"KHR_materials_emissive_strength\",\"KHR_materials_ior\",\"KHR_materials_transmission\"],\"buffers\":[{\"uri\":\"world.bin\",\"byteLength\":"+std::to_string(binary.data.size())+"}],\"bufferViews\":["+join(binary.views)+"],\"accessors\":["+join(binary.accessors)+"],\"images\":["+join(images)+"],\"textures\":["+join(textures)+"],\"samplers\":[{\"magFilter\":9729,\"minFilter\":9987,\"wrapS\":10497,\"wrapT\":10497}],\"materials\":["+join(materials)+"],\"meshes\":["+join(meshes)+"],\"nodes\":["+join(nodes)+"],\"scenes\":[{\"nodes\":["+join(nodeIndices)+"]}],\"scene\":0}");
    writeText(result.sceneFile,"{\"models\":[\"world.gltf\"],\"graph\":["+join(graph)+"]}");
    result.materialVariants=materials.size(); result.textures=textures.size();
    std::vector<std::string> warnings,sourceMaterials,sourceLights;
    for (const auto& l:result.limitations) warnings.push_back(jsonString(l));
    for (const auto& [id,m]:scene.materials) sourceMaterials.push_back("{\"id\":"+jsonString(std::to_string(id))+",\"metadata\":"+jsonString(m.metadata)+"}");
    for (const auto& [id,l]:scene.lights) sourceLights.push_back("{\"id\":"+jsonString(std::to_string(id))+",\"metadata\":"+jsonString(l.metadata)+"}");
    writeText(dir/"scene.manifest.json","{\"schema\":1,\"frameId\":"+std::to_string(scene.frameId)+",\"captureIncomplete\":"+(scene.incomplete?"true":"false")+",\"approximationsAllowed\":"+(options.allowApproximations?"true":"false")+",\"instances\":"+std::to_string(result.instances)+",\"triangles\":"+std::to_string(result.triangles)+",\"uniqueExportedVertices\":"+std::to_string(result.vertices)+",\"materialVariants\":"+std::to_string(result.materialVariants)+",\"textures\":"+std::to_string(result.textures)+",\"limitations\":["+join(warnings)+"],\"materials\":["+join(sourceMaterials)+"],\"lights\":["+join(sourceLights)+"]}");
    return result;
}
} // namespace renderer_lab
