/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix;

import java.nio.ByteBuffer;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/** Copies Java-owned extension data before its native allocation can be reused or freed. */
public final class ScenePacketExtensions {
    private static final Map<Long, Entry> ENTRIES = new HashMap<Long, Entry>();
    private static volatile int users;
    private ScenePacketExtensions() {}

    static synchronized void begin() { users++; }
    static synchronized void end() { if (--users <= 0) { users = 0; ENTRIES.clear(); } }
    public static boolean enabled() { return users != 0; }

    public static synchronized void blend(long pointer, InstanceInfoBlendEXT blend, long next) {
        if (users != 0) {
            String metadata = "";
            if (!standardOpaque(blend)) {
                metadata = ScenePacketFormat.metadata(blend).replace(
                        "\"alphaTestReferenceValue\":" + blend.alphaTestReferenceValue,
                        "\"alphaTestReferenceValue\":" + (blend.alphaTestReferenceValue & 255));
            }
            ENTRIES.put(pointer, new Entry(next, null, null, metadata));
        }
    }
    static synchronized void bones(long pointer, ByteBuffer data, int count, long next) {
        if (users != 0) ENTRIES.put(pointer, new Entry(next, ScenePacketFormat.copy(data, (long) count * 48), null, null));
    }
    static synchronized void gpu(long pointer, ByteBuffer data, int count, long next) {
        if (users != 0) ENTRIES.put(pointer, new Entry(next, null, ScenePacketFormat.copy(data, (long) count * 48), null));
    }
    public static synchronized void forget(long pointer) { if (users != 0) ENTRIES.remove(pointer); }

    static synchronized Data resolve(long pointer) {
        Data result = new Data();
        Set<Long> seen = new HashSet<Long>();
        while (pointer != 0) {
            Entry entry = ENTRIES.get(pointer);
            if (entry == null || !seen.add(pointer)) { result.complete = false; break; }
            if (entry.bones != null) result.bones = entry.bones;
            if (entry.gpu != null) result.gpu = entry.gpu;
            if (entry.blend != null) result.blend = entry.blend;
            pointer = entry.next;
        }
        return result;
    }

    static final class Data {
        byte[] bones = new byte[0], gpu = new byte[0];
        String blend = "";
        boolean complete = true;
    }
    private static final class Entry {
        final long next;
        final byte[] bones, gpu;
        final String blend;
        Entry(long next, byte[] bones, byte[] gpu, String blend) {
            this.next = next; this.bones = bones; this.gpu = gpu; this.blend = blend;
        }
    }

    private static boolean standardOpaque(InstanceInfoBlendEXT b) {
        return b.alphaTestEnabled == 0 && b.alphaBlendEnabled == 0 && b.isTextureFactorBlend == 0
                && b.tFactor == -1 && b.writeMask == 15 && b.isVertexColorBakedLighting == 0
                && b.textureColorArg1Source == InstanceInfoBlendEXT.RT_TEXTURE_ARG_TEXTURE
                && (b.textureColorArg2Source == InstanceInfoBlendEXT.RT_TEXTURE_ARG_NONE
                    || b.textureColorArg2Source == InstanceInfoBlendEXT.RT_TEXTURE_ARG_VERTEX_COLOR)
                && (b.textureColorOperation == InstanceInfoBlendEXT.RT_TEX_OP_MODULATE
                    || b.textureColorOperation == InstanceInfoBlendEXT.RT_TEX_OP_SELECT_ARG1)
                && b.textureAlphaArg1Source == InstanceInfoBlendEXT.RT_TEXTURE_ARG_TEXTURE
                && b.textureAlphaArg2Source == InstanceInfoBlendEXT.RT_TEXTURE_ARG_NONE
                && b.textureAlphaOperation == InstanceInfoBlendEXT.RT_TEX_OP_SELECT_ARG1;
    }
}
