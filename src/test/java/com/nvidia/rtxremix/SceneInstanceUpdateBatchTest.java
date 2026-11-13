/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix;

import org.junit.jupiter.api.Test;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;

import static org.junit.jupiter.api.Assertions.*;

class SceneInstanceUpdateBatchTest {
    @Test
    void ownsTransformsAndCoalescesOnlyUntilFlush() {
        List<ByteBuffer> calls = new ArrayList<>();
        SceneInstanceUpdateBatch batch = new SceneInstanceUpdateBatch((buffer, count) -> {
            assertTrue(buffer.isDirect());
            assertEquals(ByteOrder.LITTLE_ENDIAN, buffer.order());
            assertEquals(count * 72, buffer.remaining());
            ByteBuffer copy = ByteBuffer.allocate(buffer.remaining()).order(ByteOrder.LITTLE_ENDIAN);
            copy.put(buffer).flip();
            calls.add(copy);
            return (long) count << 32;
        }, 4);
        float[] transform = identity();
        transform[3] = 7;
        batch.enqueue(10, 20, 3, 1, transform);
        transform[3] = 9;
        batch.enqueue(11, 21, 4, 0, transform);
        batch.enqueue(10, 22, 5, 0, transform);
        transform[3] = 100;
        assertEquals(2, batch.pendingCount());
        assertEquals(1, batch.coalescedUpdates());
        batch.flush();
        ByteBuffer records = calls.getFirst();
        assertEquals(10, records.getLong(0));
        assertEquals(22, records.getLong(8));
        assertEquals(5, records.getInt(16));
        assertEquals(0, records.getInt(20));
        assertEquals(9, records.getFloat(36));
        assertEquals(11, records.getLong(72));
        assertEquals(9, records.getFloat(108));
        assertEquals(0, batch.pendingCount());
        batch.enqueue(10, 22, 5, 0, transform);
        batch.flush();
        assertEquals(2, calls.size());
        assertEquals(100, calls.get(1).getFloat(36));
        assertEquals(4, batch.queuedUpdates());
        assertEquals(2, batch.nativeCalls());
    }

    @Test
    void capacityFlushesBeforeAcceptingNextInstance() {
        List<Integer> counts = new ArrayList<>();
        SceneInstanceUpdateBatch batch = new SceneInstanceUpdateBatch((buffer, count) -> {
            counts.add(count);
            return (long) count << 32;
        }, 1);
        batch.enqueue(1, 2, 0, 0, identity());
        batch.enqueue(1, 3, 0, 0, identity());
        assertTrue(counts.isEmpty(), "a coalesced update fits the full batch");
        batch.enqueue(2, 3, 0, 0, identity());
        assertEquals(List.of(1), counts);
        batch.flush();
        batch.flush();
        assertEquals(List.of(1, 1), counts);
    }

    @Test
    void partialFailurePoisonsQueueWithoutReplayingAppliedUpdates() {
        int[] calls = {0};
        SceneInstanceUpdateBatch batch = new SceneInstanceUpdateBatch((buffer, count) -> {
            calls[0]++;
            return (1L << 32) | (ErrorCode.GENERAL_FAILURE & 0xffffffffL);
        }, 4);
        batch.enqueue(1, 3, 0, 0, identity());
        batch.enqueue(2, 3, 0, 0, identity());
        RemixException failure = assertThrows(RemixException.class, batch::flush);
        assertTrue(failure.getMessage().contains("failing index=1"));
        assertSame(failure, assertThrows(RemixException.class, batch::flush));
        assertSame(failure, assertThrows(RemixException.class,
                () -> batch.enqueue(3, 3, 0, 0, identity())));
        assertFalse(batch.healthy());
        assertEquals(1, calls[0]);
    }

    @Test
    void shortSuccessAndMissingNativeEntryPointAreFatal() {
        SceneInstanceUpdateBatch shortBatch = new SceneInstanceUpdateBatch((buffer, count) -> 0L, 1);
        shortBatch.enqueue(1, 2, 0, 0, identity());
        assertThrows(RemixException.class, shortBatch::flush);
        SceneInstanceUpdateBatch missing = new SceneInstanceUpdateBatch((buffer, count) -> {
            throw new UnsatisfiedLinkError("missing batch symbol");
        }, 1);
        missing.enqueue(1, 2, 0, 0, identity());
        IllegalStateException failure = assertThrows(IllegalStateException.class, missing::flush);
        assertInstanceOf(UnsatisfiedLinkError.class, failure.getCause());
        assertFalse(missing.healthy());
    }

    private static float[] identity() {
        return new float[]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    }
}
