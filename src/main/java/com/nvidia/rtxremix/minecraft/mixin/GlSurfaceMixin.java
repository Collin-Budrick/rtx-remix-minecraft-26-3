/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix.minecraft.mixin;

import com.nvidia.rtxremix.minecraft.RemixRenderer;
import com.nvidia.rtxremix.minecraft.RtxRemixMod;
import com.mojang.renderpearl.backend.opengl.GlSurface;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Replaces the OpenGL surface's buffer swap with Remix's present, so Vulkan owns the
 * swapchain whenever the renderer is initialized. The present copies Minecraft's retained
 * main target to Remix because its child window covers the host's default framebuffer.
 * Before initialization, and if it fails, the SDL swap runs unchanged.
 */
@Mixin(GlSurface.class)
public abstract class GlSurfaceMixin {

    @Inject(method = "present", at = @At("HEAD"), cancellable = true)
    private void rtxremix$presentThroughRemix(CallbackInfo ci) {
        RemixRenderer renderer = RtxRemixMod.renderer;
        if (renderer != null && renderer.isInitialized()) {
            renderer.presentFrameVk();
            ci.cancel();
        }
    }
}
