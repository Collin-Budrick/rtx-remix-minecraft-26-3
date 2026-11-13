/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix;

import java.nio.ByteBuffer;

/** Synchronous, render-thread-only JNI boundary to the actual offscreen D3D12 SDK host. */
public final class RendererLabBridge {
    public static final int OK = 0, PENDING = 1;
    public static final int SHARED_TARGET_BYTES = 104;
    private RendererLabBridge() { }

    public static native long create(String hostDll, String runtimeDirectory, String sceneDirectory,
                                     int width, int height, long adapterLuid, int flags, int aaMode);
    public static native int apply(long host, ByteBuffer packet, int byteCount);
    public static native int render(long host, long frameId, int width, int height);
    /** Fills the ABI-v1 104-byte LE RlabHostSharedTargets POD; caller reuses its direct buffer. */
    public static native int sharedTargets(long host, ByteBuffer output);
    public static native void destroy(long host);
    public static native long glAdapterLuid();
}
