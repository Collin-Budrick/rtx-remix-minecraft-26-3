/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix.minecraft;

import java.util.Locale;

/** Selects a renderer only when its in-process implementation is present. */
public record RendererBackendSelection(String requested, boolean sceneCapture, boolean fallback, String reason) {
    public static RendererBackendSelection configured() {
        return resolve(System.getProperty("rtxremix.backend", "remix"),
                Boolean.parseBoolean(System.getProperty("rtxremix.backend.fallback", "true")));
    }

    public static RendererBackendSelection resolve(String requested, boolean allowFallback) {
        String mode = requested.strip().toLowerCase(Locale.ROOT);
        return switch (mode) {
            case "remix" -> new RendererBackendSelection(mode, false, false, "selected");
            case "scene-export" -> new RendererBackendSelection(mode, true, false,
                    "capture-only; Minecraft is rendered by Remix");
            case "native-hybrid" -> new RendererBackendSelection(mode, true, false,
                    "native SDK host selected; a rendered frame is required for activation proof");
            case "restir-pt", "voxel", "gi12" -> {
                if (!allowFallback) {
                    throw new IllegalStateException("Backend " + mode
                            + " has no in-process Minecraft presentation implementation. "
                            + "The standalone SDK prototype is a separate workload; select scene-export to capture its input.");
                }
                yield new RendererBackendSelection(mode, true, true,
                        "standalone SDK prototype; no in-process presentation implementation");
            }
            default -> throw new IllegalArgumentException("Unknown rtxremix.backend: " + requested
                    + "; expected remix, scene-export, native-hybrid, restir-pt, voxel, or gi12");
        };
    }

    public String active() { return "remix"; }

    public boolean nativeHostRequested() { return requested.equals("native-hybrid") && !fallback; }

    public RendererBackendSelection unavailableNativeHost(String reason) {
        return new RendererBackendSelection(requested, true, true, reason);
    }

    public String proofJson() {
        return "{\"requested\":\"" + requested + "\",\"active\":\"remix\",\"presentation\":\"remix\","
                + "\"sceneCapture\":" + sceneCapture + ",\"nativeInProcess\":false,\"fallback\":" + fallback
                + ",\"reason\":\"" + reason + "\",\"scenePacketVersion\":1}";
    }
}
