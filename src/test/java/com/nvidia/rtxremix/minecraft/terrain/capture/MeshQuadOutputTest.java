/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix.minecraft.terrain.capture;

import net.fabricmc.fabric.api.client.renderer.v1.mesh.MeshView;
import net.fabricmc.fabric.api.client.renderer.v1.mesh.QuadEmitter;
import net.fabricmc.fabric.impl.client.indigo.renderer.mesh.MutableMeshImpl;
import net.minecraft.SharedConstants;
import net.minecraft.core.Direction;
import net.minecraft.server.Bootstrap;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import java.lang.reflect.Proxy;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;

/** Mesh-output coverage retained after 26.3 replaced the legacy RemixRenderContext. */
@Tag("fabric")
class MeshQuadOutputTest {

    @BeforeAll
    static void bootstrapMinecraft() {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
    }

    @Test
    void outputToIndigoEmitterPreservesQuadsAndClearsEmitter() {
        MutableMeshImpl source = new MutableMeshImpl();
        source.emitter().square(Direction.UP, 0, 0, 1, 1, 0).tag(42).emit();
        source.emitter().square(Direction.NORTH, 0, 0, 1, 1, 0).tag(43).emit();
        MutableMeshImpl target = new MutableMeshImpl();
        QuadEmitter emitter = target.emitter().normal(0, 1, 0, 0).tag(99);

        source.immutableCopy().outputTo(emitter);

        assertEquals(0, emitter.tag());
        assertFalse(emitter.hasNormal(0));
        assertCapturedQuads(target);
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void outputToForeignEmitterPreservesMutableAndImmutableMeshes(boolean immutable) {
        MutableMeshImpl source = new MutableMeshImpl();
        source.emitter().square(Direction.UP, 0, 0, 1, 1, 0).tag(42).emit();
        source.emitter().square(Direction.NORTH, 0, 0, 1, 1, 0).tag(43).emit();
        MutableMeshImpl target = new MutableMeshImpl();
        QuadEmitter delegate = target.emitter();
        QuadEmitter foreign = (QuadEmitter) Proxy.newProxyInstance(QuadEmitter.class.getClassLoader(),
                new Class<?>[]{QuadEmitter.class}, (proxy, method, args) -> method.invoke(delegate, args));
        MeshView mesh = immutable ? source.immutableCopy() : source;

        mesh.outputTo(foreign);

        assertCapturedQuads(target);
    }

    private static void assertCapturedQuads(MeshView mesh) {
        int[] visits = {0};
        mesh.forEach(quad -> {
            assertEquals(42 + visits[0], quad.tag());
            assertEquals(visits[0] == 0 ? Direction.UP : Direction.NORTH, quad.cullFace());
            assertFalse(quad.hasNormal(0));
            visits[0]++;
        });
        assertEquals(2, visits[0]);
    }
}
