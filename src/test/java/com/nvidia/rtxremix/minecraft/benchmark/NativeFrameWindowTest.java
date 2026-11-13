/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix.minecraft.benchmark;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

class NativeFrameWindowTest {
    @Test
    void onlyNewFenceValuesCountAndResizeStartsANewGeneration() {
        NativeFrameCounters counters = new NativeFrameCounters();
        assertTrue(counters.published(1, 7));
        assertFalse(counters.published(1, 7));
        assertTrue(counters.published(1, 8));
        assertTrue(counters.published(2, 1));
        assertEquals(3, counters.snapshot().producedFrames());
        assertEquals(1, counters.snapshot().duplicateFrames());
        assertThrows(IllegalStateException.class, () -> counters.published(2, 0));
        assertThrows(IllegalStateException.class, () -> counters.published(1, 9));
        assertTrue(counters.published(2, 2));
        assertThrows(IllegalStateException.class, () -> counters.published(2, 1));
    }

    @Test
    void recordingWindowExcludesWarmupAndRequiresExactlyOneOutputPerSample() {
        NativeFrameCounters counters = new NativeFrameCounters();
        counters.attempt(); counters.pending();
        counters.attempt(); counters.published(1, 10);
        NativeFrameWindow window = new NativeFrameWindow();
        window.begin(counters.snapshot());
        for (int i = 0; i < 3; i++) {
            counters.attempt(); counters.published(1, 11 + i);
            window.sample(counters.snapshot());
        }
        assertTrue(window.eligible(3));
        var evidence = window.json(3);
        assertTrue(evidence.get("complete").getAsBoolean());
        assertEquals(3, evidence.get("samplesWithNewFrame").getAsInt());
        assertEquals(0, evidence.getAsJsonObject("delta").get("pendingFrames").getAsInt());
        assertEquals(3, evidence.getAsJsonObject("delta").get("producedFrames").getAsInt());
        assertFalse(window.eligible(4), "missing Minecraft timing samples invalidate the window");
    }

    @Test
    void pendingFramesCannotBeCountedAsNativeRenderedFps() {
        NativeFrameCounters counters = new NativeFrameCounters();
        NativeFrameWindow window = new NativeFrameWindow();
        window.begin(counters.snapshot());
        counters.attempt(); counters.pending();
        window.sample(counters.snapshot());
        counters.attempt(); counters.published(1, 1);
        window.sample(counters.snapshot());
        assertFalse(window.eligible(2));
        var evidence = window.json(2);
        assertEquals(1, evidence.get("samplesWithoutNewFrame").getAsInt());
        assertEquals(1, evidence.getAsJsonObject("delta").get("pendingFrames").getAsInt());
        assertEquals(1, evidence.getAsJsonObject("delta").get("producedFrames").getAsInt());
    }

    @Test
    void matchingTotalsDoNotHideBurstPublicationAndARepeatedFrame() {
        NativeFrameCounters counters = new NativeFrameCounters();
        NativeFrameWindow window = new NativeFrameWindow();
        window.begin(counters.snapshot());
        counters.attempt(); counters.published(1, 1);
        counters.attempt(); counters.published(1, 2);
        window.sample(counters.snapshot());
        window.sample(counters.snapshot());
        var evidence = window.json(2);
        assertEquals(2, evidence.getAsJsonObject("delta").get("producedFrames").getAsInt());
        assertEquals(1, evidence.get("samplesWithMultipleFrames").getAsInt());
        assertEquals(1, evidence.get("samplesWithoutNewFrame").getAsInt());
        assertFalse(window.eligible(2));
    }

    @Test
    void glFailureAfterPublicationAndMissingEvidenceInvalidateTheWindow() {
        NativeFrameCounters counters = new NativeFrameCounters();
        NativeFrameWindow window = new NativeFrameWindow();
        window.begin(counters.snapshot());
        counters.attempt(); counters.published(1, 1); counters.failed();
        window.sample(counters.snapshot());
        assertFalse(window.eligible(1));
        assertEquals(1, window.json(1).getAsJsonObject("delta").get("failedFrames").getAsInt());
        window.begin(counters.snapshot());
        window.sample(null);
        assertFalse(window.json(1).get("complete").getAsBoolean());
        assertFalse(window.eligible(1));
    }

    @Test
    void duplicatePublicationAndUnreadyCaptureRemainExplicit() {
        NativeFrameCounters counters = new NativeFrameCounters();
        counters.published(1, 1);
        NativeFrameWindow window = new NativeFrameWindow();
        window.begin(counters.snapshot());
        counters.attempt(); counters.published(1, 1);
        window.sample(counters.snapshot());
        counters.attempt(); counters.unready();
        window.sample(counters.snapshot());
        assertFalse(window.eligible(2));
        assertEquals(1, window.json(2).getAsJsonObject("delta").get("duplicateFrames").getAsInt());
        assertEquals(1, window.json(2).getAsJsonObject("delta").get("unreadyFrames").getAsInt());
    }

    @Test
    void hostRecreationResetsFenceIdentityAndInvalidatesAWindowSpanningSessions() {
        NativeFrameCounters counters = new NativeFrameCounters();
        counters.beginSession();
        counters.attempt(); counters.published(5, 50);
        NativeFrameWindow window = new NativeFrameWindow();
        window.begin(counters.snapshot());
        counters.beginSession();
        counters.attempt(); counters.published(1, 1);
        window.sample(counters.snapshot());
        assertEquals(2, counters.snapshot().producedFrames());
        assertFalse(window.eligible(1));
    }
}
