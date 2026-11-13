/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix.minecraft.gl;

import org.junit.jupiter.api.Test;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

import static org.junit.jupiter.api.Assertions.*;

class D3D12SharedTargetsTest {
    @Test
    void matchingAdapterAndCompletedOutputValidateWithoutAContext() {
        assertDoesNotThrow(() -> D3D12Interop.validateTargets(targets(), 123));
    }

    @Test
    void resizeAllocationWithoutAReadyFrameCannotBeImportedAsOutput() {
        ByteBuffer targets = targets();
        targets.putLong(80, 0);
        assertThrows(IllegalStateException.class, () -> D3D12Interop.validateTargets(targets, 123));
    }

    @Test
    void incompatibleDeviceLayoutAndBorrowedHandlesAreRejected() {
        assertThrows(IllegalStateException.class, () -> D3D12Interop.validateTargets(targets(), 321));
        ByteBuffer missingHandle = targets(); missingHandle.putLong(40, -1);
        assertThrows(IllegalStateException.class, () -> D3D12Interop.validateTargets(missingHandle, 123));
        ByteBuffer wrongDepth = targets(); wrongDepth.putInt(96, 1);
        assertThrows(IllegalStateException.class, () -> D3D12Interop.validateTargets(wrongDepth, 123));
        ByteBuffer wrongByteOrder = targets().order(ByteOrder.BIG_ENDIAN);
        assertThrows(IllegalStateException.class, () -> D3D12Interop.validateTargets(wrongByteOrder, 123));
        ByteBuffer shortOutput = targets(); shortOutput.limit(80);
        assertThrows(IllegalStateException.class, () -> D3D12Interop.validateTargets(shortOutput, 123));
    }

    private static ByteBuffer targets() {
        ByteBuffer data = ByteBuffer.allocateDirect(104).order(ByteOrder.LITTLE_ENDIAN);
        data.putInt(0, 104).putInt(4, 1).putInt(8, 1280).putInt(12, 720);
        data.putLong(16, 1).putLong(24, 123).putLong(32, 1).putLong(40, 2);
        data.putLong(48, 65536).putLong(56, 65536).putLong(64, 3).putLong(72, 4).putLong(80, 1);
        data.putInt(88, 28).putInt(92, 41).putInt(96, 0);
        return data;
    }
}
