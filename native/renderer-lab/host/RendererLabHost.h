/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdint.h>
#ifndef RLAB_HOST_EXPORT
#ifdef _WIN32
#define RLAB_HOST_EXPORT __declspec(dllexport)
#else
#define RLAB_HOST_EXPORT
#endif
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define RLAB_HOST_ABI_VERSION 1u
enum RlabHostStatus { RLAB_HOST_OK=0, RLAB_HOST_PENDING=1, RLAB_HOST_ERROR=-1 };
enum RlabHostFlags { RLAB_HOST_ALLOW_APPROXIMATIONS=1u, RLAB_HOST_DEBUG_VALIDATION=2u };
enum RlabHostAaMode { RLAB_HOST_AA_NONE=0, RLAB_HOST_AA_TAA=1, RLAB_HOST_AA_SR=2, RLAB_HOST_AA_RR=3 };
typedef struct RlabHostCreateInfo {
    uint32_t structSize,abiVersion,width,height;
    uint64_t adapterLuid; /* Required DXGI LUID, LowPart | uint64_t(HighPart)<<32. */
    uint32_t flags,aaMode;
    const char* runtimeDirectoryUtf8; /* SDK bin directory with shaders, DLLs, Assets. */
    const char* sceneDirectoryUtf8;   /* Disposable exported scene cache, owned by caller. */
} RlabHostCreateInfo;
typedef struct RlabHostSharedTargets {
    uint32_t structSize,abiVersion,width,height;
    uint64_t generation,adapterLuid;
    uint64_t colorResourceHandle,depthResourceHandle;
    uint64_t colorAllocationBytes,depthAllocationBytes;
    uint64_t dxReadyFenceHandle,glDoneFenceHandle,readyFenceValue;
    uint32_t colorDxgiFormat,depthDxgiFormat;
    uint32_t depthConvention,reserved; /* 0: OpenGL projected [0,1], sky=1. */
} RlabHostSharedTargets;
/* All calls belong to Minecraft's render thread. Packet bytes are copied/decoded
 * before Apply returns. Render submits actual SDK GPU work; PENDING means no
 * new complete image was published. GetSharedTargets returns borrowed Win32
 * handles owned by the host: GL imports them but must not CloseHandle them.
 * Wait dxReady=readyFenceValue before sampling; signal glDone=same value after
 * color+depth composite commands. Host waits glDone before reusing resources.
 * Release GL imports before resize/destroy. Width/height changes are requested
 * by Render; generation changes only when the output allocations change. */
/* The JNI owner pins this module for the JVM lifetime. Destroy releases the
 * host's GPU/resources, not the DLL; replacing native code requires restart. */
RLAB_HOST_EXPORT uint32_t RendererLabHostAbiVersion(void);
RLAB_HOST_EXPORT int32_t RendererLabHostCreate(const RlabHostCreateInfo*,void** host,char* error,uint32_t errorCapacity);
RLAB_HOST_EXPORT int32_t RendererLabHostApply(void* host,const void* packetBytes,uint64_t byteCount,char* error,uint32_t errorCapacity);
RLAB_HOST_EXPORT int32_t RendererLabHostRender(void* host,uint64_t frameId,uint32_t width,uint32_t height,char* error,uint32_t errorCapacity);
RLAB_HOST_EXPORT int32_t RendererLabHostGetSharedTargets(void* host,RlabHostSharedTargets*,char* error,uint32_t errorCapacity);
RLAB_HOST_EXPORT void RendererLabHostDestroy(void* host);
#ifdef __cplusplus
}
#endif
