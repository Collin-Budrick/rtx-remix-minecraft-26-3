/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix.minecraft;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

class RendererBackendSelectionTest {
    @Test
    void defaultRemixDoesNotPayForSceneCapture() {
        var selection = RendererBackendSelection.resolve("remix", true);
        assertEquals("remix", selection.active());
        assertFalse(selection.sceneCapture());
        assertFalse(selection.fallback());
    }

    @Test
    void standaloneExperimentsNeverClaimInProcessRendering() {
        for (String id : new String[]{"restir-pt", "voxel", "gi12"}) {
            var selection = RendererBackendSelection.resolve(id, true);
            assertEquals(id, selection.requested());
            assertEquals("remix", selection.active());
            assertTrue(selection.sceneCapture());
            assertTrue(selection.fallback());
            assertTrue(selection.proofJson().contains("\"presentation\":\"remix\""));
            assertTrue(selection.proofJson().contains("\"nativeInProcess\":false"));
            assertThrows(IllegalStateException.class, () -> RendererBackendSelection.resolve(id, false));
        }
    }

    @Test
    void nativeHostSelectionStillRequiresRuntimeFrameProof() {
        var selection = RendererBackendSelection.resolve("native-hybrid", false);
        assertTrue(selection.nativeHostRequested());
        assertFalse(selection.fallback());
        assertTrue(selection.proofJson().contains("\"nativeInProcess\":false"));
        assertEquals("remix", selection.active(), "selection alone is never renderer activation proof");
    }

    @Test
    void explicitExportIsCaptureOnlyAndUnknownModesFail() {
        var selection = RendererBackendSelection.resolve(" SCENE-EXPORT ", false);
        assertTrue(selection.sceneCapture());
        assertFalse(selection.fallback());
        assertEquals("remix", selection.active());
        assertTrue(selection.reason().contains("capture-only"));
        assertThrows(IllegalArgumentException.class,
                () -> RendererBackendSelection.resolve("made-up-renderer", true));
    }
}
