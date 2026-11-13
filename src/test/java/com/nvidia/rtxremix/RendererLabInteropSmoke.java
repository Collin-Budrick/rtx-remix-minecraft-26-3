/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix;

import com.nvidia.rtxremix.minecraft.gl.D3D12Interop;
import org.lwjgl.glfw.GLFWErrorCallback;
import org.lwjgl.opengl.GL;

import javax.imageio.ImageIO;
import java.awt.image.BufferedImage;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;

import static org.lwjgl.glfw.GLFW.*;
import static org.lwjgl.opengl.GL11.*;
import static org.lwjgl.opengl.GL45.glGetTextureImage;
import static org.lwjgl.system.MemoryUtil.*;

/** Explicit GPU diagnostic: actual JNI host, shared color/depth, two fences, resize and recreation.
 * Creates a hidden GL context; never launches or controls Minecraft or Modrinth.
 */
public final class RendererLabInteropSmoke {
    private RendererLabInteropSmoke() { }

    public static void main(String[] args) throws Exception {
        if (args.length != 5) throw new IllegalArgumentException("Expected bridge DLL, host DLL, runtime, packet, output");
        Path output = Path.of(args[4]).toAbsolutePath();
        if (Files.exists(output)) throw new IllegalArgumentException("Diagnostic output must be a new directory");
        Files.createDirectories(output);
        System.load(Path.of(args[0]).toAbsolutePath().toString());
        GLFWErrorCallback errorCallback = GLFWErrorCallback.createPrint(System.err);
        glfwSetErrorCallback(errorCallback);
        long window = 0, host = 0;
        D3D12Interop interop = null;
        ByteBuffer packet = null, targets = null;
        try {
            require(glfwInit(), "GLFW initialization failed");
            glfwDefaultWindowHints();
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
            window = glfwCreateWindow(640, 360, "Renderer lab offscreen diagnostic", 0, 0);
            require(window != 0, "Hidden GL context creation failed");
            glfwMakeContextCurrent(window);
            GL.createCapabilities();
            long luid = RendererLabBridge.glAdapterLuid();
            require(luid != 0, "OpenGL returned a zero adapter LUID");
            System.out.println("RTX_RENDERER_LAB_GL vendor=" + glGetString(GL_VENDOR)
                    + " renderer=" + glGetString(GL_RENDERER) + " luid=" + Long.toUnsignedString(luid));
            byte[] bytes = Files.readAllBytes(Path.of(args[3]));
            packet = memAlloc(bytes.length).put(bytes).flip();
            targets = memAlloc(RendererLabBridge.SHARED_TARGET_BYTES).order(ByteOrder.LITTLE_ENDIAN);
            boolean rejectedReadOnly = false;
            try { RendererLabBridge.sharedTargets(0, targets.asReadOnlyBuffer()); }
            catch (IllegalStateException expected) { rejectedReadOnly = expected.getMessage().contains("writable"); }
            require(rejectedReadOnly, "JNI did not reject a read-only output buffer before native writes");
            StringBuilder results = new StringBuilder();
            int aaMode = Integer.getInteger("rtxremix.smoke.aaMode", 0);
            require(aaMode >= 0 && aaMode <= 3, "Smoke AA mode must be 0 (none), 1 (TAA), 2 (SR), or 3 (RR)");
            for (int session = 0; session < 2; session++) {
                int flags = Boolean.getBoolean("rtxremix.smoke.debugValidation") ? 2 : 0;
                host = RendererLabBridge.create(Path.of(args[1]).toAbsolutePath().toString(),
                        Path.of(args[2]).toAbsolutePath().toString(),
                        output.resolve("scene-" + session).toString(), 640, 360, luid, flags, aaMode);
                require(host != 0, "Host creation returned a null handle");
                require(RendererLabBridge.apply(host, packet, bytes.length) == RendererLabBridge.OK, "Scene apply failed");
                interop = new D3D12Interop(luid);
                long lastGeneration = 0, lastFence = 0;
                for (int phase = 0; phase < 2; phase++) {
                    int width = phase == 0 ? 640 : 320, height = phase == 0 ? 360 : 180;
                    // Borrowed GL resources must be gone before the host replaces its allocations.
                    if (phase != 0) interop.close();
                    long deadline = System.nanoTime() + Duration.ofMinutes(2).toNanos();
                    int completed = 0;
                    while (completed < 12) {
                        require(System.nanoTime() < deadline, "Timed out waiting for a complete native frame");
                        int status = RendererLabBridge.render(host, completed, width, height);
                        if (status == RendererLabBridge.PENDING) { Thread.sleep(5); continue; }
                        require(status == RendererLabBridge.OK, "Unexpected native render status");
                        require(RendererLabBridge.sharedTargets(host, targets) == RendererLabBridge.OK, "Shared targets failed");
                        require(targets.getInt(8) == width && targets.getInt(12) == height, "Wrong output size");
                        long generation = targets.getLong(16), fence = targets.getLong(80);
                        require(fence > lastFence && generation >= lastGeneration, "Native output fence or generation regressed");
                        if (phase != 0 && completed == 0) require(generation > lastGeneration, "Resize did not replace allocations");
                        lastGeneration = generation; lastFence = fence;
                        interop.acquire(targets);
                        try {
                            if (completed == 11) {
                                if (!results.isEmpty()) results.append(',');
                                results.append(capture(interop, output.resolve("session-" + session + "-phase-" + phase + ".png")));
                            }
                        } finally { interop.release(); }
                        completed++;
                    }
                }
                interop.close(); interop = null;
                RendererLabBridge.destroy(host); host = 0;
            }
            String json = "{\"passed\":true,\"workload\":\"synthetic-sdk-smoke\",\"minecraftBenchmark\":false,"
                    + "\"frameGeneration\":false,\"aaMode\":" + aaMode + ",\"adapterLuid\":\"" + Long.toUnsignedString(luid)
                    + "\",\"captures\":[" + results + "]}";
            Files.writeString(output.resolve("interop.manifest.json"), json);
            System.out.println("RTX_RENDERER_LAB_INTEROP_PASS " + json);
        } finally {
            if (interop != null) interop.close();
            if (host != 0) RendererLabBridge.destroy(host);
            if (packet != null) memFree(packet);
            if (targets != null) memFree(targets);
            GL.setCapabilities(null);
            if (window != 0) glfwDestroyWindow(window);
            glfwTerminate();
            glfwSetErrorCallback(null);
            errorCallback.free();
        }
    }

    private static String capture(D3D12Interop interop, Path path) throws Exception {
        int width = interop.width(), height = interop.height(), pixels = width * height;
        ByteBuffer color = memAlloc(pixels * 4);
        FloatBuffer depth = memAllocFloat(pixels);
        try {
            glGetTextureImage(interop.colorTexture(), 0, GL_RGBA, GL_UNSIGNED_BYTE, color);
            glGetTextureImage(interop.depthTexture(), 0, GL_RED, GL_FLOAT, depth);
            require(glGetError() == GL_NO_ERROR, "Shared texture readback failed");
            BufferedImage image = new BufferedImage(width, height, BufferedImage.TYPE_INT_ARGB);
            int minimum = 255, maximum = 0, geometry = 0;
            for (int i = 0; i < pixels; i++) {
                int r = Byte.toUnsignedInt(color.get(i * 4)), g = Byte.toUnsignedInt(color.get(i * 4 + 1));
                int b = Byte.toUnsignedInt(color.get(i * 4 + 2));
                minimum = Math.min(minimum, Math.min(r, Math.min(g, b)));
                maximum = Math.max(maximum, Math.max(r, Math.max(g, b)));
                image.setRGB(i % width, i / width, 0xff000000 | r << 16 | g << 8 | b);
                float z = depth.get(i);
                require(Float.isFinite(z) && z >= 0 && z <= 1, "Invalid projected depth sample: " + z);
                if (z < .999999f) geometry++;
            }
            require(maximum - minimum >= 8, "Native output is blank or constant");
            require(geometry > pixels / 100, "Shared depth contains no useful geometry");
            require(ImageIO.write(image, "PNG", path.toFile()), "PNG writer unavailable");
            return "{\"file\":\"" + path.getFileName() + "\",\"width\":" + width + ",\"height\":" + height
                    + ",\"colorMin\":" + minimum + ",\"colorMax\":" + maximum + ",\"geometryPixels\":" + geometry + "}";
        } finally { memFree(color); memFree(depth); }
    }

    private static void require(boolean passed, String message) {
        if (!passed) throw new IllegalStateException(message);
    }
}
