/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix;

import org.junit.jupiter.api.Test;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

import static org.junit.jupiter.api.Assertions.*;

class VirtualSceneApiTest {
    @Test
    void resourceAndFrameOperationsUseOwnedSceneWithoutAnyNativeGpuEntryPoint() throws Exception {
        RemixAPI api = virtualApi();
        ScenePacketRecorder capture = api.getSceneCapture();
        try {
            TextureFromMemoryInfo texture = new TextureFromMemoryInfo();
            texture.width = 1; texture.height = 1; texture.format = TextureFormat.RGBA8_UNORM;
            texture.pixelData = ByteBuffer.allocateDirect(4);
            long textureId = api.createTextureFromMemory(texture);
            MaterialInfo material = new MaterialInfo();
            material.albedoTexture = textureId; material.opaqueExt = new MaterialInfoOpaqueEXT();
            long materialId = api.createMaterial(material);
            MeshInfoSurfaceTriangles surface = new MeshInfoSurfaceTriangles();
            surface.material = materialId; surface.vertexCount = 3; surface.indexCount = 3;
            surface.vertexBuffer = ByteBuffer.allocateDirect(192);
            surface.indexBuffer = ByteBuffer.allocateDirect(12).order(ByteOrder.LITTLE_ENDIAN);
            surface.indexBuffer.putInt(0).putInt(1).putInt(2);
            MeshInfo mesh = new MeshInfo(); mesh.surfaces = new MeshInfoSurfaceTriangles[]{surface};
            long meshId = api.createMesh(mesh);
            float[] transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            long instanceId = api.sceneInstanceAdd(meshId, transform, 0, 0, 0);
            transform[3] = 4;
            api.sceneInstanceUpdate(instanceId, meshId, transform, 0, 0, 0);
            LightInfo light = new LightInfo(); light.sphereExt = new LightInfoSphereEXT();
            long lightId = api.createLight(light);
            api.drawLightInstance(lightId);
            CameraInfo camera = new CameraInfo(); camera.parameterizedExt = new CameraInfoParameterizedEXT();
            api.setSceneCaptureViewport(1280, 720);
            api.setupCamera(camera);
            api.submit(null);
            assertTrue(capture.ready());
            assertEquals(1, capture.instanceCount());
            assertEquals(1, capture.meshCount());
            assertEquals(1, capture.textureCount());
            var packet = capture.packet(true);
            assertTrue(packet.bytes().isDirect());
            assertEquals(ByteOrder.LITTLE_ENDIAN, packet.bytes().order());
            assertEquals(6, packet.bytes().getInt(12));
            capture.acknowledge(packet);
            assertThrows(IllegalArgumentException.class, () -> capture.acknowledge(packet));
            api.sceneInstanceRemove(instanceId);
            api.destroyMesh(meshId);
            api.destroyMaterial(materialId);
            api.destroyTexture(textureId);
            api.destroyLight(lightId);
            assertEquals(0, capture.instanceCount());
            assertEquals(0, capture.meshCount());
            assertEquals(0, capture.textureCount());
        } finally { api.shutdown(); }
        assertFalse(api.isInitialized());
    }

    @Test
    void unappliedRuntimeFeaturesNeverReturnFakeSupportOrCallRemix() throws Exception {
        RemixAPI api = virtualApi();
        try {
            assertFalse(api.isConfigLayersSupported());
            assertThrows(UnsupportedOperationException.class, () -> api.acquireConfigLayer("water.conf", 1, 1));
            assertThrows(UnsupportedOperationException.class, () -> api.startup(new StartupInfo()));
            assertThrows(UnsupportedOperationException.class, api::dxvkGetInteropInfo);
            assertThrows(UnsupportedOperationException.class, () -> api.present(new PresentInfo()));
            assertEquals("0", api.getConfigVariableResolved("rtx.showUI"));
            assertThrows(UnsupportedOperationException.class, () -> api.setConfigVariable("rtx.showUI", "1"));
            api.setConfigVariable("rtx.tonemappingMode", "1");
            assertThrows(RemixException.class, () -> api.getConfigVariableResolved("rtx.tonemappingMode"));
            api.setCameraMediumMaterial(99);
            assertTrue(api.unappliedNativeFeatures().contains("camera-water-medium"));
            assertTrue(api.unappliedNativeFeatures().contains("remix-config-options"));
            assertThrows(IllegalArgumentException.class, () -> api.sceneInstanceRemove(123));
        } finally { api.shutdown(); }
    }

    @Test
    void acknowledgingPacketKeepsUpdatesProducedAfterItsSnapshot() {
        try (ScenePacketRecorder capture = new ScenePacketRecorder()) {
            capture.textureFile(1, "original.png");
            var old = capture.packet(true);
            capture.textureFile(1, "updated.png");
            capture.acknowledge(old);
            var current = capture.packet(false);
            assertEquals(1, current.bytes().getInt(12));
            capture.acknowledge(current);
            assertEquals(0, capture.packet(false).bytes().getInt(12));
        }
    }

    /** Avoids even loading the JNI library: any accidental GPU/native call fails this test. */
    private static RemixAPI virtualApi() throws Exception {
        var constructor = RemixAPI.class.getDeclaredConstructor(boolean.class);
        constructor.setAccessible(true);
        RemixAPI api = constructor.newInstance(true);
        var initialized = RemixAPI.class.getDeclaredField("initialized");
        initialized.setAccessible(true);
        initialized.setBoolean(api, true);
        api.enableSceneCapture(new ScenePacketRecorder());
        return api;
    }
}
