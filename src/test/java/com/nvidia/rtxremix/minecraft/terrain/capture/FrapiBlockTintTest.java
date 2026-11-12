/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix.minecraft.terrain.capture;

import org.junit.jupiter.api.Test;

import java.lang.reflect.Method;

import static org.junit.jupiter.api.Assertions.assertEquals;

/** The 26.3 capture path resolves tint indices directly, without the old render context. */
class FrapiBlockTintTest {

    @Test
    void theTintMultipliesEachChannelAndKeepsTheVertexAlpha() throws Exception {
        assertEquals(0xFF7FB238, tintFrapiColor(0xFFFFFFFF, 0x7FB238));
        // 0x80 * 0x80 / 0xFF rounds down to 0x40, as Indigo's ColorHelper does.
        assertEquals(0x40404040, tintFrapiColor(0x40808080, 0x808080));
        assertEquals(0x80336699, tintFrapiColor(0x80336699, 0xFFFFFF));
    }

    @Test
    void aBlockWithoutAColorProviderKeepsTheVertexColor() throws Exception {
        // BlockColors answers -1 for a block with no provider registered.
        assertEquals(0x80336699, tintFrapiColor(0x80336699, -1));
    }

    private static int tintFrapiColor(int color, int tint) throws Exception {
        Method multiply = BlockQuadEmitter.class.getDeclaredMethod("multiplyRgb", int.class, int.class);
        multiply.setAccessible(true);
        return (int) multiply.invoke(null, color, tint);
    }
}
