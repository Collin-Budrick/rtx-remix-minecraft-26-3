/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix;

import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.security.MessageDigest;

/** Deterministic, JNI-free input for the real cross-language decoder and standalone SDK smoke runs. */
public final class RendererLabFixture {
    private static final long CHECKER = 101, WHITE = 102;
    private static final long TEXTURED = 201, WALL = 202, EMISSIVE = 203;
    private static final long ROOM = 301, EMITTER = 302, CUBE = 303;
    private static final int WIDTH = 1280, HEIGHT = 720, RECORDS = 16;

    private RendererLabFixture() { }

    public static void main(String[] args) throws Exception {
        if (args.length != 1) throw new IllegalArgumentException("Expected one output .rslb path");
        Path output = Paths.get(args[0]).toAbsolutePath().normalize();
        ScenePacketRecorder.ExportSummary summary;
        try (ScenePacketRecorder capture = new ScenePacketRecorder()) {
            capture.texture(CHECKER, texture(2, 2, new int[]{0xffc8c8c8, 0xff287fb8, 0xff287fb8, 0xffc8c8c8}));
            capture.texture(WHITE, texture(1, 1, new int[]{0xffffffff}));
            capture.material(TEXTURED, material(CHECKER, .85f, .85f, .85f, 0));
            capture.material(WALL, material(WHITE, .65f, .68f, .72f, 0));
            capture.material(EMISSIVE, material(WHITE, .03f, .03f, .03f, 6));
            capture.mesh(ROOM, room());
            capture.mesh(EMITTER, emitter());
            capture.mesh(CUBE, cube());
            capture.instance(401, ROOM, transform(0, 0, 0), 0, 0, 0);
            capture.instance(402, EMITTER, transform(0, 0, 0), 0, 1, 0);
            capture.instance(403, CUBE, transform(-2, .5f, -1), 0, 0, 0);
            capture.instance(404, CUBE, transform(2, .5f, -2), 0, 0, 0);
            // Behind the camera: a retained contributor must survive capture without a frustum filter.
            capture.instance(405, CUBE, transform(0, .5f, 20), 0, 0, 0);

            LightInfo sun = new LightInfo(501, .8f, .75f, .65f);
            sun.distantExt = new LightInfoDistantEXT();
            sun.distantExt.direction = new Float3D(.3f, -.9f, -.31622776f);
            sun.distantExt.angularDiameterDegrees = .5f;
            capture.light(501, sun);
            capture.lightDrawn(501);
            LightInfo bulb = new LightInfo(502, 40, 32, 24);
            bulb.sphereExt = new LightInfoSphereEXT();
            bulb.sphereExt.position = new Float3D(0, 3, 1);
            bulb.sphereExt.radius = .2f;
            capture.light(502, bulb);
            capture.lightDrawn(502);
            capture.viewport(WIDTH, HEIGHT);
            capture.camera(camera());
            capture.frameSubmitted();
            require(capture.ready() && capture.complete(), "fixture capture must be ready and complete");
            require(capture.meshCount() == 3 && capture.instanceCount() == 5 && capture.textureCount() == 2,
                    "retained fixture resource counts changed");
            summary = capture.writeSnapshot(output);
        }
        require(summary.frame == 1 && summary.meshes == 3 && summary.instances == 5
                && summary.textures == 2 && summary.complete, "committed fixture summary changed");
        byte[] header = new byte[ScenePacketFormat.HEADER_BYTES];
        try (InputStream input = Files.newInputStream(output)) {
            int read = 0;
            while (read < header.length) {
                int count = input.read(header, read, header.length - read);
                if (count < 0) throw new IllegalStateException("Truncated fixture header");
                read += count;
            }
        }
        ByteBuffer packet = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        require(packet.getInt() == ScenePacketFormat.MAGIC && packet.getInt() == 1, "fixture format changed");
        require(packet.getInt() == ScenePacketFormat.FULL_RESET && packet.getInt() == RECORDS,
                "fixture must be a complete full reset with the expected records");
        require(packet.getLong() == 1 && Integer.toUnsignedLong(packet.getInt()) == Files.size(output) - header.length
                && packet.getInt() == 0, "fixture packet length or frame changed");
        ByteBuffer contents = ByteBuffer.wrap(Files.readAllBytes(output)).order(ByteOrder.LITTLE_ENDIAN);
        contents.position(ScenePacketFormat.HEADER_BYTES);
        int[] counts = new int[14];
        boolean offCameraPresent = false;
        for (int i = 0; i < RECORDS; i++) {
            int type = contents.getInt(), length = contents.getInt();
            require(type > 0 && type < counts.length && length >= 0 && length <= contents.remaining(),
                    "invalid fixture record bounds");
            counts[type]++;
            if (type == ScenePacketFormat.INSTANCE && contents.getLong(contents.position()) == 405) {
                offCameraPresent = contents.getFloat(contents.position() + 68) == 20;
            }
            contents.position(contents.position() + length);
        }
        require(!contents.hasRemaining() && counts[ScenePacketFormat.TEXTURE] == 2
                && counts[ScenePacketFormat.MATERIAL] == 3 && counts[ScenePacketFormat.MESH] == 3
                && counts[ScenePacketFormat.INSTANCE] == 5 && counts[ScenePacketFormat.LIGHT] == 2
                && counts[ScenePacketFormat.CAMERA] == 1 && offCameraPresent,
                "committed fixture records or off-camera contributor changed");
        System.out.println("RTX_RENDERER_LAB_FIXTURE {\"path\":\"" + output.toString().replace("\\", "\\\\").replace("\"", "\\\"")
                + "\",\"packetVersion\":1,\"frame\":1,\"records\":16,\"meshes\":3,\"instances\":5,\"materials\":3,"
                + "\"textures\":2,\"lights\":2,\"trianglesUnique\":19,\"trianglesInstanced\":43,"
                + "\"offCameraInstances\":1,\"width\":1280,\"height\":720,\"complete\":true,"
                + "\"workload\":\"synthetic-sdk-smoke\",\"sha256\":\"" + sha256(output) + "\"}");
    }

    private static TextureFromMemoryInfo texture(int width, int height, int[] argb) {
        TextureFromMemoryInfo info = new TextureFromMemoryInfo();
        info.hash = width * 31L + height;
        info.width = width; info.height = height; info.format = TextureFormat.RGBA8_SRGB;
        info.pixelData = ByteBuffer.allocateDirect(argb.length * 4).order(ByteOrder.LITTLE_ENDIAN);
        for (int color : argb) info.pixelData.put((byte) (color >>> 16)).put((byte) (color >>> 8))
                .put((byte) color).put((byte) (color >>> 24));
        info.pixelData.flip();
        return info;
    }

    private static MaterialInfo material(long albedo, float r, float g, float b, float emission) {
        MaterialInfo info = new MaterialInfo(albedo * 100 + (long) (emission * 10));
        info.albedoTexture = albedo;
        info.opaqueExt = new MaterialInfoOpaqueEXT();
        info.opaqueExt.albedoConstant = new Float3D(r, g, b);
        info.opaqueExt.roughnessConstant = .7f;
        if (emission > 0) {
            info.emissiveTexture = WHITE;
            info.emissiveColorConstant = new Float3D(1, .45f, .12f);
            info.emissiveIntensity = emission;
        }
        return info;
    }

    private static MeshInfo room() {
        return mesh(ROOM,
                quad(TEXTURED, new float[]{0, 1, 0}, new float[][]{
                        {-6, 0, -6}, {-6, 0, 6}, {6, 0, 6}, {6, 0, -6}}),
                quad(WALL, new float[]{0, 0, 1}, new float[][]{
                        {-6, 0, -6}, {6, 0, -6}, {6, 4, -6}, {-6, 4, -6}}),
                quad(WALL, new float[]{1, 0, 0}, new float[][]{
                        {-6, 0, 6}, {-6, 0, -6}, {-6, 4, -6}, {-6, 4, 6}}));
    }

    private static MeshInfo emitter() {
        MeshInfoSurfaceTriangles surface = surface(EMISSIVE, 3, new int[]{0, 1, 2});
        vertex(surface.vertexBuffer, -1, 2, -5.9f, 0, 0, 1, 0, 0);
        vertex(surface.vertexBuffer, 1, 2, -5.9f, 0, 0, 1, 1, 0);
        vertex(surface.vertexBuffer, 0, 3.5f, -5.9f, 0, 0, 1, .5f, 1);
        surface.vertexBuffer.flip();
        return mesh(EMITTER, surface);
    }

    private static MeshInfo cube() {
        return mesh(CUBE,
                quad(TEXTURED, new float[]{0, 0, 1}, new float[][]{
                        {-.5f, -.5f, .5f}, {.5f, -.5f, .5f}, {.5f, .5f, .5f}, {-.5f, .5f, .5f}}),
                quad(TEXTURED, new float[]{0, 0, -1}, new float[][]{
                        {.5f, -.5f, -.5f}, {-.5f, -.5f, -.5f}, {-.5f, .5f, -.5f}, {.5f, .5f, -.5f}}),
                quad(TEXTURED, new float[]{1, 0, 0}, new float[][]{
                        {.5f, -.5f, .5f}, {.5f, -.5f, -.5f}, {.5f, .5f, -.5f}, {.5f, .5f, .5f}}),
                quad(TEXTURED, new float[]{-1, 0, 0}, new float[][]{
                        {-.5f, -.5f, -.5f}, {-.5f, -.5f, .5f}, {-.5f, .5f, .5f}, {-.5f, .5f, -.5f}}),
                quad(TEXTURED, new float[]{0, 1, 0}, new float[][]{
                        {-.5f, .5f, .5f}, {.5f, .5f, .5f}, {.5f, .5f, -.5f}, {-.5f, .5f, -.5f}}),
                quad(TEXTURED, new float[]{0, -1, 0}, new float[][]{
                        {-.5f, -.5f, -.5f}, {.5f, -.5f, -.5f}, {.5f, -.5f, .5f}, {-.5f, -.5f, .5f}}));
    }

    private static MeshInfo mesh(long hash, MeshInfoSurfaceTriangles... surfaces) {
        MeshInfo info = new MeshInfo(hash);
        info.surfaces = surfaces;
        return info;
    }

    private static MeshInfoSurfaceTriangles quad(long material, float[] normal, float[][] positions) {
        MeshInfoSurfaceTriangles surface = surface(material, 4, new int[]{0, 1, 2, 0, 2, 3});
        float[][] uv = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (int i = 0; i < 4; i++) vertex(surface.vertexBuffer, positions[i][0], positions[i][1], positions[i][2],
                normal[0], normal[1], normal[2], uv[i][0], uv[i][1]);
        surface.vertexBuffer.flip();
        return surface;
    }

    private static MeshInfoSurfaceTriangles surface(long material, int vertices, int[] indices) {
        MeshInfoSurfaceTriangles surface = new MeshInfoSurfaceTriangles();
        surface.material = material; surface.vertexCount = vertices; surface.indexCount = indices.length;
        surface.vertexBuffer = ByteBuffer.allocateDirect(vertices * HardcodedVertex.STRIDE).order(ByteOrder.LITTLE_ENDIAN);
        surface.indexBuffer = ByteBuffer.allocateDirect(indices.length * 4).order(ByteOrder.LITTLE_ENDIAN);
        for (int index : indices) surface.indexBuffer.putInt(index);
        surface.indexBuffer.flip();
        return surface;
    }

    private static void vertex(ByteBuffer buffer, float x, float y, float z, float nx, float ny, float nz, float u, float v) {
        HardcodedVertex.create(x, y, z, nx, ny, nz, u, v, 0xffffffff).writeTo(buffer);
    }

    private static float[] transform(float x, float y, float z) {
        return new float[]{1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z};
    }

    private static CameraInfo camera() {
        CameraInfo info = new CameraInfo();
        CameraInfoParameterizedEXT camera = new CameraInfoParameterizedEXT();
        camera.position = new Float3D(0, 2.5f, 8);
        float c = (float) Math.sqrt(.96);
        camera.forward = new Float3D(0, -.2f, -c);
        camera.up = new Float3D(0, c, -.2f);
        camera.right = new Float3D(1, 0, 0);
        camera.fovYInDegrees = 55; camera.aspect = (float) WIDTH / HEIGHT;
        camera.nearPlane = .1f; camera.farPlane = 100;
        info.parameterizedExt = camera;
        info.view = new float[]{1, 0, 0, 0, 0, c, .2f, 0, 0, -.2f, c, 0,
                0, -(c * 2.5f - .2f * 8), -(.2f * 2.5f + c * 8), 1};
        float f = 1f / (float) Math.tan(Math.toRadians(camera.fovYInDegrees) * .5);
        float near = camera.nearPlane, far = camera.farPlane;
        info.projection = new float[]{f / camera.aspect, 0, 0, 0, 0, f, 0, 0,
                0, 0, (far + near) / (near - far), -1, 0, 0, 2 * far * near / (near - far), 0};
        return info;
    }

    private static String sha256(Path path) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream input = Files.newInputStream(path)) {
            byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) != -1) digest.update(buffer, 0, count);
        }
        StringBuilder hex = new StringBuilder();
        for (byte value : digest.digest()) hex.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        return hex.toString();
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new IllegalStateException(message);
    }
}
