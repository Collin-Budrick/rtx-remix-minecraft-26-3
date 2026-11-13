/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix.minecraft.benchmark;

/** Render-thread evidence of new SDK GPU outputs, independent of Minecraft's presentation rate. */
public final class NativeFrameCounters {
    private long attempts, produced, pending, unready, duplicates, failed;
    private long generation = -1, readyFenceValue;
    private long session;

    public void beginSession() { session++; generation = -1; readyFenceValue = 0; }

    public void attempt() { attempts++; }
    public void pending() { pending++; }
    public void unready() { unready++; }
    public void failed() { failed++; }

    public boolean published(long newGeneration, long newReadyFenceValue) {
        if (newGeneration < 0 || newReadyFenceValue <= 0 || newGeneration < generation
                || newGeneration == generation && Long.compareUnsigned(newReadyFenceValue, readyFenceValue) < 0) {
            throw new IllegalStateException("Native shared output generation or ready fence moved backwards or is uninitialized");
        }
        if (newGeneration == generation && newReadyFenceValue == readyFenceValue) {
            duplicates++;
            return false;
        }
        generation = newGeneration;
        readyFenceValue = newReadyFenceValue;
        produced++;
        return true;
    }

    public Snapshot snapshot() { return new Snapshot(attempts, produced, pending, unready, duplicates, failed, generation, readyFenceValue, session); }

    public record Snapshot(long renderAttempts, long producedFrames, long pendingFrames, long unreadyFrames,
                           long duplicateFrames, long failedFrames, long lastGeneration, long lastReadyFenceValue, long session) { }
}
