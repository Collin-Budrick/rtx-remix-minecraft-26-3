/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <gl/GL.h>
#include <jni.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#define RLAB_HOST_EXPORT
#include "../../native/renderer-lab/host/RendererLabHost.h"

static_assert(sizeof(RlabHostSharedTargets) == 104, "Shared-target JNI wire layout changed");

namespace {
void fail(JNIEnv* env, const std::string& message) {
    if (env->ExceptionCheck()) return;
    auto type = env->FindClass("java/lang/IllegalStateException");
    if (type) env->ThrowNew(type, message.c_str());
}

std::wstring wide(JNIEnv* env, jstring value) {
    if (!value) throw std::invalid_argument("Renderer host paths must not be null");
    auto length = env->GetStringLength(value);
    auto data = env->GetStringChars(value, nullptr);
    if (!data) throw std::runtime_error("Could not read renderer host path");
    std::wstring result(reinterpret_cast<const wchar_t*>(data), length);
    env->ReleaseStringChars(value, data);
    if (result.find(L'\0') != std::wstring::npos || !std::filesystem::path(result).is_absolute())
        throw std::invalid_argument("Renderer host paths must be absolute and contain no NUL");
    return result;
}

std::string utf8(const std::wstring& value) {
    auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!length) throw std::invalid_argument("Invalid Unicode renderer host path");
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

template<class Function>
Function procedure(HMODULE library, const char* name) {
    auto address = GetProcAddress(library, name);
    if (!address) throw std::runtime_error(std::string("Renderer host is missing ") + name);
    return reinterpret_cast<Function>(address);
}

struct Binding {
    HMODULE library{};
    DLL_DIRECTORY_COOKIE directory{};
    void* host{};
    DWORD thread = GetCurrentThreadId();
    decltype(&RendererLabHostCreate) create{};
    decltype(&RendererLabHostApply) apply{};
    decltype(&RendererLabHostRender) render{};
    decltype(&RendererLabHostGetSharedTargets) shared{};
    decltype(&RendererLabHostDestroy) destroy{};

    ~Binding() {
        if (host && destroy) destroy(host);
        if (library) FreeLibrary(library);
        if (directory) RemoveDllDirectory(directory);
    }
};
std::mutex bindingsMutex;
std::map<jlong, std::unique_ptr<Binding>> bindings;
std::wstring pinnedHostPath;
jlong nextBinding = 1;

Binding& binding(jlong id) {
    auto found = bindings.find(id);
    if (found == bindings.end()) throw std::invalid_argument("Renderer host handle is closed or invalid");
    if (found->second->thread != GetCurrentThreadId())
        throw std::runtime_error("Renderer host calls must stay on their creating render thread");
    return *found->second;
}

int status(int result, const char* error) {
    if (result == RLAB_HOST_ERROR)
        throw std::runtime_error(error && *error ? error : "Renderer host operation failed");
    if (result != RLAB_HOST_OK && result != RLAB_HOST_PENDING)
        throw std::runtime_error("Renderer host returned an invalid status");
    return result;
}
} // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RendererLabBridge_create(JNIEnv* env, jclass,
    jstring dll, jstring runtime, jstring scene, jint width, jint height,
    jlong luid, jint flags, jint aaMode)
{
    try {
        if (width <= 0 || height <= 0 || !luid) throw std::invalid_argument("Invalid renderer dimensions or adapter LUID");
        auto dllPath = wide(env, dll);
        auto runtimePath = wide(env, runtime);
        auto scenePath = wide(env, scene);
        auto runtimeUtf8 = utf8(runtimePath);
        auto sceneUtf8 = utf8(scenePath);
        std::lock_guard<std::mutex> guard(bindingsMutex);
        if (!bindings.empty()) throw std::runtime_error("Only one experimental renderer host may be active");
        dllPath = std::filesystem::weakly_canonical(dllPath).native();
        if (!pinnedHostPath.empty() && _wcsicmp(pinnedHostPath.c_str(), dllPath.c_str()) != 0)
            throw std::runtime_error("Restart Minecraft before selecting a different native renderer host DLL");
        auto host = std::make_unique<Binding>();
        host->directory = AddDllDirectory(runtimePath.c_str());
        if (!host->directory) throw std::runtime_error("Cannot register renderer runtime DLL directory");
        host->library = LoadLibraryExW(dllPath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!host->library) throw std::runtime_error("Cannot load renderer host DLL (Windows error " + std::to_string(GetLastError()) + ")");
        // Like Java-loaded natives, the SDK module lives for the JVM lifetime.
        // Its imported Tracy global joins worker threads during DLL detach;
        // unloading it inside FreeLibrary's loader lock deadlocks. HostDestroy
        // still releases each host's GPU resources before the binding closes.
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(host->library), &pinned))
            throw std::runtime_error("Cannot retain renderer host module for the JVM lifetime");
        pinnedHostPath = dllPath;
        auto version = procedure<decltype(&RendererLabHostAbiVersion)>(host->library, "RendererLabHostAbiVersion");
        if (version() != RLAB_HOST_ABI_VERSION) throw std::runtime_error("Renderer host ABI version mismatch");
        host->create = procedure<decltype(host->create)>(host->library, "RendererLabHostCreate");
        host->apply = procedure<decltype(host->apply)>(host->library, "RendererLabHostApply");
        host->render = procedure<decltype(host->render)>(host->library, "RendererLabHostRender");
        host->shared = procedure<decltype(host->shared)>(host->library, "RendererLabHostGetSharedTargets");
        host->destroy = procedure<decltype(host->destroy)>(host->library, "RendererLabHostDestroy");
        RlabHostCreateInfo info{};
        info.structSize = sizeof(info);
        info.abiVersion = RLAB_HOST_ABI_VERSION;
        info.width = static_cast<uint32_t>(width);
        info.height = static_cast<uint32_t>(height);
        info.adapterLuid = static_cast<uint64_t>(luid);
        info.flags = static_cast<uint32_t>(flags);
        info.aaMode = static_cast<uint32_t>(aaMode);
        info.runtimeDirectoryUtf8 = runtimeUtf8.c_str();
        info.sceneDirectoryUtf8 = sceneUtf8.c_str();
        char error[2048]{};
        if (status(host->create(&info, &host->host, error, sizeof(error)), error) != RLAB_HOST_OK || !host->host)
            throw std::runtime_error("Renderer host did not finish initialization");
        auto id = nextBinding++;
        bindings.emplace(id, std::move(host));
        return id;
    } catch (const std::exception& error) { fail(env, error.what()); return 0; }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RendererLabBridge_apply(JNIEnv* env, jclass, jlong id, jobject packet, jint byteCount)
{
    try {
        auto capacity = packet ? env->GetDirectBufferCapacity(packet) : -1;
        auto data = packet ? env->GetDirectBufferAddress(packet) : nullptr;
        if (!data || byteCount < 32 || byteCount > capacity)
            throw std::invalid_argument("Scene packets require a direct buffer with enough bytes from offset zero");
        std::lock_guard<std::mutex> guard(bindingsMutex);
        auto& host = binding(id);
        char error[2048]{};
        return status(host.apply(host.host, data, static_cast<uint64_t>(byteCount), error, sizeof(error)), error);
    } catch (const std::exception& error) { fail(env, error.what()); return RLAB_HOST_ERROR; }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RendererLabBridge_render(JNIEnv* env, jclass, jlong id, jlong frameId, jint width, jint height)
{
    try {
        if (frameId < 0 || width <= 0 || height <= 0) throw std::invalid_argument("Invalid native frame request");
        std::lock_guard<std::mutex> guard(bindingsMutex);
        auto& host = binding(id);
        char error[2048]{};
        return status(host.render(host.host, static_cast<uint64_t>(frameId), width, height, error, sizeof(error)), error);
    } catch (const std::exception& error) { fail(env, error.what()); return RLAB_HOST_ERROR; }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_nvidia_rtxremix_RendererLabBridge_sharedTargets(JNIEnv* env, jclass, jlong id, jobject buffer)
{
    try {
        auto capacity = buffer ? env->GetDirectBufferCapacity(buffer) : -1;
        auto data = buffer ? env->GetDirectBufferAddress(buffer) : nullptr;
        if (!data || capacity < static_cast<jlong>(sizeof(RlabHostSharedTargets)))
            throw std::invalid_argument("Shared targets require a direct buffer of at least 104 bytes");
        auto bufferClass = env->GetObjectClass(buffer);
        if (!bufferClass) return RLAB_HOST_ERROR;
        auto isReadOnly = env->GetMethodID(bufferClass, "isReadOnly", "()Z");
        if (!isReadOnly) { env->DeleteLocalRef(bufferClass); return RLAB_HOST_ERROR; }
        auto readOnly = env->CallBooleanMethod(buffer, isReadOnly);
        env->DeleteLocalRef(bufferClass);
        if (env->ExceptionCheck()) return RLAB_HOST_ERROR;
        if (readOnly) throw std::invalid_argument("Shared targets require a writable output buffer");
        std::lock_guard<std::mutex> guard(bindingsMutex);
        auto& host = binding(id);
        RlabHostSharedTargets targets{};
        targets.structSize = sizeof(targets);
        targets.abiVersion = RLAB_HOST_ABI_VERSION;
        char error[2048]{};
        auto result = status(host.shared(host.host, &targets, error, sizeof(error)), error);
        if (result == RLAB_HOST_OK) std::memcpy(data, &targets, sizeof(targets));
        return result;
    } catch (const std::exception& error) { fail(env, error.what()); return RLAB_HOST_ERROR; }
}

extern "C" JNIEXPORT void JNICALL
Java_com_nvidia_rtxremix_RendererLabBridge_destroy(JNIEnv* env, jclass, jlong id)
{
    try {
        std::lock_guard<std::mutex> guard(bindingsMutex);
        auto found = bindings.find(id);
        if (found == bindings.end()) return;
        binding(id);
        bindings.erase(found);
    } catch (const std::exception& error) { fail(env, error.what()); }
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_nvidia_rtxremix_RendererLabBridge_glAdapterLuid(JNIEnv* env, jclass)
{
    try {
        if (!wglGetCurrentContext()) throw std::runtime_error("An OpenGL context is required to identify the adapter");
        using GetUnsignedBytes = void(APIENTRY*)(GLenum, unsigned char*);
        auto address = wglGetProcAddress("glGetUnsignedBytevEXT");
        if (!address || address == reinterpret_cast<PROC>(1) || address == reinterpret_cast<PROC>(2) ||
            address == reinterpret_cast<PROC>(3) || address == reinterpret_cast<PROC>(-1))
            throw std::runtime_error("GL_EXT_memory_object adapter identification is unavailable");
        uint64_t luid = 0;
        reinterpret_cast<GetUnsignedBytes>(address)(0x9599 /* GL_DEVICE_LUID_EXT */, reinterpret_cast<unsigned char*>(&luid));
        if (!luid) throw std::runtime_error("OpenGL did not expose a valid DXGI adapter LUID");
        return static_cast<jlong>(luid);
    } catch (const std::exception& error) { fail(env, error.what()); return 0; }
}
