/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;

/** Owned, bounded JNI transport for retained-instance updates with no borrowed extension pointers. */
public final class SceneInstanceUpdateBatch {
    public static final int STRIDE = 72;

    @FunctionalInterface
    public interface Submitter {
        /** High 32 bits are the number applied; low 32 bits are the Remix error code. */
        long submit(ByteBuffer records, int count);
    }

    private final Submitter submitter;
    private final ByteBuffer records;
    private final long[] instanceIds;
    private final int[] slots;
    private int count;
    private RuntimeException failure;
    private long calls, queued, coalesced;

    public SceneInstanceUpdateBatch(Submitter submitter, int capacity) {
        if (submitter == null || capacity < 1 || capacity > 65536) {
            throw new IllegalArgumentException("Batch capacity must be 1..65536 and submitter is required");
        }
        this.submitter = submitter;
        records = ByteBuffer.allocateDirect(capacity * STRIDE).order(ByteOrder.LITTLE_ENDIAN);
        instanceIds = new long[capacity];
        int tableSize = 2;
        while (tableSize < capacity * 2) tableSize <<= 1;
        slots = new int[tableSize];
    }

    public void enqueue(long instance, long mesh, int flags, int doubleSided, float[] transform) {
        checkHealthy();
        if (instance == 0 || mesh == 0 || transform == null || transform.length < 12) {
            throw new IllegalArgumentException("A batch update requires live handles and a 3x4 transform");
        }
        int bucket = bucket(instance);
        int record = slots[bucket] - 1;
        if (record < 0 && count == instanceIds.length) {
            flush();
            bucket = bucket(instance);
        }
        record = slots[bucket] - 1;
        if (record < 0) {
            record = count++;
            instanceIds[record] = instance;
            slots[bucket] = record + 1;
        } else coalesced++;
        int offset = record * STRIDE;
        records.putLong(offset, instance).putLong(offset + 8, mesh);
        records.putInt(offset + 16, flags).putInt(offset + 20, doubleSided);
        for (int i = 0; i < 12; i++) records.putFloat(offset + 24 + i * 4, transform[i]);
        queued++;
    }

    public void flush() {
        checkHealthy();
        if (count == 0) return;
        ByteBuffer view = records.duplicate().order(ByteOrder.LITTLE_ENDIAN);
        view.position(0).limit(count * STRIDE);
        long result;
        try {
            result = submitter.submit(view, count);
            calls++;
        } catch (RuntimeException | LinkageError error) {
            failure = new IllegalStateException("Retained-instance batch transport failed before an applied count was returned", error);
            throw failure;
        }
        int applied = (int) (result >>> 32);
        int error = (int) result;
        if (error != ErrorCode.SUCCESS || applied != count) {
            failure = new RemixException("Retained-instance batch failed after " + applied + " of " + count
                    + " updates; failing index=" + applied + ". The queue is poisoned and will not replay applied updates.",
                    error != ErrorCode.SUCCESS ? error : ErrorCode.GENERAL_FAILURE);
            throw failure;
        }
        count = 0;
        Arrays.fill(slots, 0);
    }

    public int pendingCount() { return count; }
    public long nativeCalls() { return calls; }
    public long queuedUpdates() { return queued; }
    public long coalescedUpdates() { return coalesced; }
    public boolean healthy() { return failure == null; }

    private int bucket(long instance) {
        long mixed = instance ^ (instance >>> 33);
        mixed *= 0xff51afd7ed558ccdL;
        int bucket = (int) (mixed ^ (mixed >>> 33)) & (slots.length - 1);
        while (slots[bucket] != 0 && instanceIds[slots[bucket] - 1] != instance) {
            bucket = (bucket + 1) & (slots.length - 1);
        }
        return bucket;
    }

    private void checkHealthy() { if (failure != null) throw failure; }
}
