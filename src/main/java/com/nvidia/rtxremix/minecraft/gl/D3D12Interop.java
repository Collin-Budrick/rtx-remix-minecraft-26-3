/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix.minecraft.gl;

import org.lwjgl.opengl.GL;

import java.nio.ByteBuffer;

import static org.lwjgl.opengl.EXTMemoryObject.*;
import static org.lwjgl.opengl.EXTMemoryObjectWin32.*;
import static org.lwjgl.opengl.EXTSemaphore.*;
import static org.lwjgl.opengl.EXTSemaphoreWin32.*;
import static org.lwjgl.opengl.GL11.*;
import static org.lwjgl.opengl.GL12.GL_CLAMP_TO_EDGE;
import static org.lwjgl.opengl.GL30.GL_R32F;
import static org.lwjgl.opengl.GL45.*;

/** Borrows SDK-owned D3D12 resources and fences; the Minecraft window remains an ordinary GL surface. */
public final class D3D12Interop implements AutoCloseable {
    private static final int[] NO_BUFFERS = new int[0];
    private static final int[] LAYOUTS = {GL_LAYOUT_GENERAL_EXT, GL_LAYOUT_GENERAL_EXT};
    private static final int[] HANDLE_OFFSETS = {32, 40, 64, 72};
    private final long adapterLuid;
    private final int[] memories = new int[2], textures = new int[2];
    private long generation = -1, readyHandle, doneHandle, acquiredValue;
    private long colorHandle, depthHandle, colorBytes, depthBytes;
    private int readySemaphore, doneSemaphore, width, height;
    private boolean acquired;

    public D3D12Interop(long adapterLuid) {
        var caps = GL.getCapabilities();
        if (!caps.GL_EXT_memory_object || !caps.GL_EXT_memory_object_win32
                || !caps.GL_EXT_semaphore || !caps.GL_EXT_semaphore_win32) {
            throw new IllegalStateException("Native renderer needs GL EXT memory-object and Win32 semaphore imports");
        }
        this.adapterLuid = adapterLuid;
    }

    public void acquire(ByteBuffer targets) {
        if (acquired) throw new IllegalStateException("Native frame was not released before the next acquire");
        validateTargets(targets, adapterLuid);
        while (glGetError() != GL_NO_ERROR) { }
        int newWidth = targets.getInt(8), newHeight = targets.getInt(12);
        long newGeneration = targets.getLong(16), newReady = targets.getLong(64), newDone = targets.getLong(72);
        if (generation != newGeneration) {
            close();
            width = newWidth; height = newHeight;
            if (width <= 0 || height <= 0 || newReady == 0 || newDone == 0) {
                throw new IllegalStateException("Native shared targets are incomplete");
            }
            try {
                textures[0] = importImage(0, targets.getLong(32), targets.getLong(48), GL_RGBA8, GL_LINEAR);
                textures[1] = importImage(1, targets.getLong(40), targets.getLong(56), GL_R32F, GL_NEAREST);
                readySemaphore = importFence(newReady);
                doneSemaphore = importFence(newDone);
                readyHandle = newReady; doneHandle = newDone; generation = newGeneration;
                colorHandle = targets.getLong(32); depthHandle = targets.getLong(40);
                colorBytes = targets.getLong(48); depthBytes = targets.getLong(56);
            } catch (RuntimeException failure) { close(); throw failure; }
        } else if (width != newWidth || height != newHeight || readyHandle != newReady || doneHandle != newDone
                || colorHandle != targets.getLong(32) || depthHandle != targets.getLong(40)
                || colorBytes != targets.getLong(48) || depthBytes != targets.getLong(56)) {
            throw new IllegalStateException("Native resources changed without a generation increment");
        }
        acquiredValue = targets.getLong(80);
        setFenceValue(readySemaphore, acquiredValue);
        glWaitSemaphoreEXT(readySemaphore, NO_BUFFERS, textures, LAYOUTS);
        check("wait for D3D12 frame");
        acquired = true;
    }

    static void validateTargets(ByteBuffer targets, long expectedLuid) {
        if (!targets.isDirect() || targets.limit() < 104 || targets.order() != java.nio.ByteOrder.LITTLE_ENDIAN
                || targets.getInt(0) != 104 || targets.getInt(4) != 1 || targets.getLong(24) != expectedLuid
                || targets.getInt(88) != 28 || targets.getInt(92) != 41 || targets.getInt(96) != 0
                || targets.getInt(8) <= 0 || targets.getInt(12) <= 0 || targets.getLong(16) < 0
                || targets.getLong(48) <= 0 || targets.getLong(56) <= 0 || targets.getLong(80) <= 0) {
            throw new IllegalStateException("Native shared targets have incompatible ABI, adapter, image extent/format, depth, or no completed frame");
        }
        for (int offset : HANDLE_OFFSETS) {
            if (targets.getLong(offset) == 0 || targets.getLong(offset) == -1) {
                throw new IllegalStateException("Native shared targets contain a missing Win32 resource or fence handle");
            }
        }
    }

    /** Runs after every GL use of color/depth, including lens, weather, and GUI submission. */
    public void release() {
        if (!acquired) return;
        setFenceValue(doneSemaphore, acquiredValue);
        glSignalSemaphoreEXT(doneSemaphore, NO_BUFFERS, textures, LAYOUTS);
        check("release D3D12 frame");
        glFlush();
        acquired = false;
    }

    public int colorTexture() { return acquired ? textures[0] : 0; }
    public int depthTexture() { return acquired ? textures[1] : 0; }
    public int width() { return width; }
    public int height() { return height; }

    private int importImage(int index, long handle, long bytes, int format, int filter) {
        if (handle == 0 || bytes <= 0) throw new IllegalArgumentException("Missing native shared image");
        memories[index] = glCreateMemoryObjectsEXT();
        glMemoryObjectParameteriEXT(memories[index], GL_DEDICATED_MEMORY_OBJECT_EXT, GL_TRUE);
        glImportMemoryWin32HandleEXT(memories[index], bytes, GL_HANDLE_TYPE_D3D12_RESOURCE_EXT, handle);
        check("import D3D12 memory");
        int texture = glCreateTextures(GL_TEXTURE_2D);
        textures[index] = texture;
        glTextureStorageMem2DEXT(texture, 1, format, width, height, memories[index], 0);
        glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, filter);
        glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, filter);
        glTextureParameteri(texture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(texture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        check("import D3D12 texture");
        return texture;
    }

    private int importFence(long handle) {
        int semaphore = glGenSemaphoresEXT();
        try {
            glImportSemaphoreWin32HandleEXT(semaphore, GL_HANDLE_TYPE_D3D12_FENCE_EXT, handle);
            check("import D3D12 fence");
            return semaphore;
        } catch (RuntimeException failure) { glDeleteSemaphoresEXT(semaphore); throw failure; }
    }

    private static void setFenceValue(int semaphore, long value) {
        glSemaphoreParameterui64EXT(semaphore, GL_D3D12_FENCE_VALUE_EXT, value);
        check("set D3D12 fence value");
    }

    private static void check(String operation) {
        int error = glGetError();
        if (error != GL_NO_ERROR) throw new IllegalStateException(operation + ": GL error 0x" + Integer.toHexString(error));
    }

    /** Must precede a host resize or destruction; imported Win32 handles remain owned by the host. */
    @Override public void close() {
        RuntimeException failure = null;
        try { release(); glFinish(); }
        catch (RuntimeException | LinkageError error) { failure = cleanupFailure(failure, error); }
        for (int i = 0; i < textures.length; i++) {
            try { if (textures[i] != 0) glDeleteTextures(textures[i]); }
            catch (RuntimeException | LinkageError error) { failure = cleanupFailure(failure, error); }
            try { if (memories[i] != 0) glDeleteMemoryObjectsEXT(memories[i]); }
            catch (RuntimeException | LinkageError error) { failure = cleanupFailure(failure, error); }
            textures[i] = 0; memories[i] = 0;
        }
        try { if (readySemaphore != 0) glDeleteSemaphoresEXT(readySemaphore); }
        catch (RuntimeException | LinkageError error) { failure = cleanupFailure(failure, error); }
        try { if (doneSemaphore != 0) glDeleteSemaphoresEXT(doneSemaphore); }
        catch (RuntimeException | LinkageError error) { failure = cleanupFailure(failure, error); }
        readySemaphore = 0; doneSemaphore = 0; generation = -1;
        readyHandle = 0; doneHandle = 0; width = 0; height = 0;
        acquired = false;
        if (failure != null) throw failure;
    }

    private static RuntimeException cleanupFailure(RuntimeException previous, Throwable error) {
        RuntimeException failure = error instanceof RuntimeException runtime ? runtime
                : new IllegalStateException("Native GL import cleanup failed", error);
        if (previous == null) return failure;
        previous.addSuppressed(failure);
        return previous;
    }
}
