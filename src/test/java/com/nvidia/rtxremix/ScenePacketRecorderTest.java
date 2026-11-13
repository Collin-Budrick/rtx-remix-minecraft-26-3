/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

import static org.junit.jupiter.api.Assertions.*;

class ScenePacketRecorderTest {
    @TempDir Path directory;

    @Test
    void snapshotOwnsBuffersAndKeepsRetainedInstancesAcrossFrames() throws Exception {
        try (ScenePacketRecorder capture = new ScenePacketRecorder()) {
            TextureFromMemoryInfo texture = texture(2, 1, new byte[]{1, 2, 3, 4, 5, 6, 7, 8});
            texture.pixelData.position(4); // JNI addresses the base, independent of Java position.
            capture.texture(3, texture);
            MeshInfo mesh = triangle();
            mesh.surfaces[0].vertexBuffer.putFloat(0, 12.5f);
            capture.mesh(5, mesh);
            float[] transform = identity();
            transform[3] = 30;
            capture.instance(7, 5, transform, 9, 1, 0);
            texture.pixelData.put(0, (byte) 99);
            mesh.surfaces[0].vertexBuffer.putFloat(0, 99);
            transform[3] = 99;
            capture.viewport(2560, 1440);
            CameraInfo camera = new CameraInfo();
            camera.parameterizedExt = new CameraInfoParameterizedEXT();
            capture.camera(camera);
            capture.frameSubmitted();
            capture.frameSubmitted();
            assertTrue(capture.ready());
            Path output = directory.resolve("scene.rslb");
            capture.writeSnapshot(output);
            Packet packet = read(output);
            assertEquals(ScenePacketFormat.FULL_RESET, packet.flags);
            assertEquals(2, packet.frame);
            assertEquals(4, packet.records.size());
            assertEquals(1, packet.one(ScenePacketFormat.TEXTURE).get(36));
            assertEquals(12.5f, packet.one(ScenePacketFormat.MESH).getFloat(60));
            assertEquals(30, packet.one(ScenePacketFormat.INSTANCE).getFloat(36));
            assertEquals(2560, packet.one(ScenePacketFormat.CAMERA).getInt(8));
            assertEquals(1440, packet.one(ScenePacketFormat.CAMERA).getInt(12));
            capture.writeUpdates(output);
            assertTrue(read(output).records.isEmpty());
        }
    }

    @Test
    void paddedTexturePatchesBecomeOneOwnedTextureUpdate() throws Exception {
        try (ScenePacketRecorder capture = new ScenePacketRecorder()) {
            capture.texture(3, texture(2, 2, new byte[16]));
            Path output = directory.resolve("updates.rslb");
            capture.writeSnapshot(output);
            TexturePatch patch = new TexturePatch();
            patch.dstX = 1; patch.width = 1; patch.height = 2; patch.rowPitch = 8;
            patch.pixelData = bytes(new byte[]{1, 2, 3, 4, 0, 0, 0, 0, 5, 6, 7, 8, 0, 0, 0, 0});
            capture.texturePatches(patches(3, patch));
            patch.pixelData.put(0, (byte) 99);
            capture.writeUpdates(output);
            Packet packet = read(output);
            assertEquals(0, packet.flags);
            assertEquals(1, packet.records.size());
            ByteBuffer texture = packet.one(ScenePacketFormat.TEXTURE);
            assertEquals(0, texture.get(36));
            assertEquals(1, texture.get(40));
            assertEquals(5, texture.get(48));
            assertEquals(8, texture.get(51));
        }
    }

    @Test
    void destroyingFileTextureDiscardsItsPendingPatches() throws Exception {
        try (ScenePacketRecorder capture = new ScenePacketRecorder()) {
            Path output = directory.resolve("updates.rslb");
            capture.textureFile(3, "atlas.png");
            capture.writeSnapshot(output);
            TexturePatch patch = new TexturePatch();
            patch.width = 1; patch.height = 1; patch.pixelData = bytes(new byte[4]);
            capture.texturePatches(patches(3, patch));
            capture.removeTexture(3);
            capture.writeUpdates(output);
            Packet packet = read(output);
            assertEquals(1, packet.records.size());
            assertEquals(3, packet.one(ScenePacketFormat.REMOVE_TEXTURE).getLong());
        }
    }

    @Test
    void failedWriteDoesNotAcknowledgeDirtyRecords() throws Exception {
        try (ScenePacketRecorder capture = new ScenePacketRecorder()) {
            capture.texture(3, texture(1, 1, new byte[4]));
            Path regularFile = directory.resolve("not-a-directory");
            Files.writeString(regularFile, "occupied");
            assertThrows(IOException.class, () -> capture.writeUpdates(regularFile.resolve("scene.rslb")));
            Path output = directory.resolve("updates.rslb");
            capture.writeUpdates(output);
            assertEquals(1, read(output).records.size());
            capture.writeUpdates(output);
            assertTrue(read(output).records.isEmpty());
        }
    }

    @Test
    void copiesBonePoseBeforeNativeChainIsFreedAndFlagsUnknownExtensions() throws Exception {
        try (ScenePacketRecorder capture = new ScenePacketRecorder()) {
            ByteBuffer bones = ByteBuffer.allocateDirect(48).order(ByteOrder.LITTLE_ENDIAN);
            bones.putFloat(0, 2);
            ScenePacketExtensions.bones(123, bones, 1, 0);
            capture.instance(7, 5, identity(), 0, 0, 123);
            ScenePacketExtensions.forget(123);
            bones.putFloat(0, 9);
            Path output = directory.resolve("scene.rslb");
            capture.writeSnapshot(output);
            ByteBuffer instance = read(output).one(ScenePacketFormat.INSTANCE);
            assertEquals(1, instance.getInt(72));
            assertEquals(2, instance.getFloat(88));
            capture.instance(7, 5, identity(), 0, 0, 999);
            capture.writeSnapshot(output);
            assertFalse(capture.complete());
            assertEquals(ScenePacketFormat.FULL_RESET | ScenePacketFormat.INCOMPLETE, read(output).flags);
            capture.instance(7, 5, identity(), 0, 0, 0);
            assertTrue(capture.complete());
        }
    }

    @Test
    void onlySubmittedLightsAndCurrentTransientInstancesSurvive() throws Exception {
        try (ScenePacketRecorder capture = new ScenePacketRecorder()) {
            LightInfo light = new LightInfo();
            light.sphereExt = new LightInfoSphereEXT();
            capture.light(9, light);
            capture.lightDrawn(9);
            capture.transientInstance(5, identity(), 0, 0, 0);
            capture.frameSubmitted();
            Path output = directory.resolve("scene.rslb");
            capture.writeSnapshot(output);
            assertEquals(2, read(output).records.size());
            capture.frameSubmitted();
            capture.writeSnapshot(output);
            assertTrue(read(output).records.isEmpty());
        }
    }

    @Test
    void lightRemovalAndRedrawCoalesceToItsFinalStateWithinAnExportInterval() throws Exception {
        try (ScenePacketRecorder capture = new ScenePacketRecorder()) {
            LightInfo light = new LightInfo();
            light.sphereExt = new LightInfoSphereEXT();
            capture.light(9, light);
            capture.lightDrawn(9);
            capture.frameSubmitted();
            Path output = directory.resolve("lights.rslb");
            capture.writeSnapshot(output);
            capture.frameSubmitted(); // Absent this frame, so the dirty state contains a removal.
            capture.lightDrawn(9);
            capture.frameSubmitted(); // Redrawn before the next export.
            capture.writeUpdates(output);
            Packet packet = read(output);
            assertEquals(1, packet.records.size());
            assertEquals(9, packet.one(ScenePacketFormat.LIGHT).getLong());

            capture.light(10, light);
            capture.lightDrawn(10);
            capture.frameSubmitted();
            capture.removeLight(10);
            light.radiance = new Float3D(2, 3, 4);
            capture.light(10, light);
            capture.lightDrawn(10);
            capture.frameSubmitted();
            capture.writeUpdates(output);
            packet = read(output);
            assertEquals(2, packet.records.size()); // Light9 was removed; light10 was recreated.
            assertEquals(9, packet.one(ScenePacketFormat.REMOVE_LIGHT).getLong());
            ByteBuffer recreated = packet.one(ScenePacketFormat.LIGHT);
            assertEquals(10, recreated.getLong());
            assertEquals(2, recreated.getFloat(24));
        }
    }

    @Test
    void rejectsIncompleteSkinningArraysAndFailedRecorderExports() {
        try (ScenePacketRecorder capture = new ScenePacketRecorder()) {
            MeshInfo mesh = triangle();
            mesh.surfaces[0].skinning_hasvalue = true;
            mesh.surfaces[0].skinning_bonesPerVertex = 2;
            mesh.surfaces[0].skinning_blendWeightsCount = 5;
            mesh.surfaces[0].skinning_blendIndicesCount = 6;
            assertThrows(IllegalArgumentException.class, () -> capture.mesh(5, mesh));
            capture.failed(new IllegalArgumentException("capture copy failed"));
            assertFalse(capture.ready());
            assertFalse(capture.complete());
            assertThrows(IOException.class, () -> capture.writeSnapshot(directory.resolve("scene.rslb")));
        }
    }

    @Test
    void worldClearDropsUnsubmittedTransientsAndLightsBeforeRejoin() throws Exception {
        try (ScenePacketRecorder capture = new ScenePacketRecorder()) {
            capture.texture(3, texture(1, 1, new byte[4]));
            capture.mesh(5, triangle());
            capture.instance(7, 5, identity(), 0, 0, 0);
            capture.transientInstance(5, identity(), 0, 0, 0); // A failed frame never reached submit.
            LightInfo light = new LightInfo(); light.sphereExt = new LightInfoSphereEXT();
            capture.light(9, light);
            capture.lightDrawn(9);
            capture.viewport(1280, 720);
            CameraInfo camera = new CameraInfo(); camera.parameterizedExt = new CameraInfoParameterizedEXT();
            capture.camera(camera);
            capture.clearScene();
            assertEquals(0, capture.instanceCount());
            assertEquals(1, capture.meshCount());
            assertEquals(1, capture.textureCount());
            assertFalse(capture.ready());
            capture.instance(10, 5, identity(), 0, 0, 0);
            capture.transientInstance(5, identity(), 0, 0, 0);
            capture.camera(camera);
            capture.frameSubmitted();
            Path output = directory.resolve("rejoined.rslb");
            capture.writeSnapshot(output);
            Packet packet = read(output);
            assertEquals(2, packet.records.stream().filter(record -> record.type == ScenePacketFormat.INSTANCE).count());
            assertEquals(0, packet.records.stream().filter(record -> record.type == ScenePacketFormat.LIGHT).count());
            assertTrue(packet.records.stream().filter(record -> record.type == ScenePacketFormat.INSTANCE)
                    .noneMatch(record -> record.data.getLong(0) == 7 || record.data.getLong(0) == Long.MIN_VALUE + 1));
        }
    }

    private static TextureFromMemoryInfo texture(int width, int height, byte[] pixels) {
        TextureFromMemoryInfo info = new TextureFromMemoryInfo();
        info.width = width; info.height = height;
        info.format = TextureFormat.RGBA8_UNORM; info.pixelData = bytes(pixels);
        return info;
    }

    private static TexturePatchInfo patches(long texture, TexturePatch patch) {
        TextureBatchEntry entry = new TextureBatchEntry();
        entry.handle = texture; entry.patches = new TexturePatch[]{patch};
        TexturePatchInfo info = new TexturePatchInfo();
        info.entries = new TextureBatchEntry[]{entry};
        return info;
    }

    private static MeshInfo triangle() {
        MeshInfoSurfaceTriangles surface = new MeshInfoSurfaceTriangles();
        surface.vertexCount = 3; surface.indexCount = 3;
        surface.vertexBuffer = ByteBuffer.allocateDirect(3 * HardcodedVertex.STRIDE).order(ByteOrder.LITTLE_ENDIAN);
        surface.indexBuffer = ByteBuffer.allocateDirect(12).order(ByteOrder.LITTLE_ENDIAN);
        surface.indexBuffer.putInt(0).putInt(1).putInt(2);
        MeshInfo mesh = new MeshInfo();
        mesh.surfaces = new MeshInfoSurfaceTriangles[]{surface};
        return mesh;
    }

    private static ByteBuffer bytes(byte[] data) {
        ByteBuffer buffer = ByteBuffer.allocateDirect(data.length).order(ByteOrder.LITTLE_ENDIAN);
        buffer.put(data).flip();
        return buffer;
    }

    private static float[] identity() { return new float[]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}; }

    private static Packet read(Path path) throws IOException {
        ByteBuffer input = ByteBuffer.wrap(Files.readAllBytes(path)).order(ByteOrder.LITTLE_ENDIAN);
        assertEquals(ScenePacketFormat.MAGIC, input.getInt());
        assertEquals(ScenePacketFormat.VERSION, input.getInt());
        int flags = input.getInt(), count = input.getInt();
        long frame = input.getLong();
        int payload = input.getInt();
        assertEquals(0, input.getInt());
        assertEquals(payload, input.remaining());
        List<Tlv> records = new ArrayList<>();
        for (int i = 0; i < count; i++) {
            int type = input.getInt(), length = input.getInt();
            ByteBuffer data = input.slice().order(ByteOrder.LITTLE_ENDIAN);
            data.limit(length);
            records.add(new Tlv(type, data));
            input.position(input.position() + length);
        }
        assertFalse(input.hasRemaining());
        return new Packet(flags, frame, records);
    }

    private record Tlv(int type, ByteBuffer data) { }
    private record Packet(int flags, long frame, List<Tlv> records) {
        ByteBuffer one(int type) {
            return records.stream().filter(record -> record.type == type).findFirst().orElseThrow().data;
        }
    }
}
