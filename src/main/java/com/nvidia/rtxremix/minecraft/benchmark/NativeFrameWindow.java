/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
package com.nvidia.rtxremix.minecraft.benchmark;

import com.google.gson.JsonObject;

/** Matches each recorded full Minecraft frame to exactly one new native GPU output. */
final class NativeFrameWindow {
    private NativeFrameCounters.Snapshot start, end;
    private long samples, newFrameSamples, missingFrameSamples, multipleFrameSamples;
    private boolean missingEvidence;

    void begin(NativeFrameCounters.Snapshot counters) {
        start = counters; end = counters;
        samples = newFrameSamples = missingFrameSamples = multipleFrameSamples = 0;
        missingEvidence = counters == null;
    }

    void sample(NativeFrameCounters.Snapshot counters) {
        samples++;
        if (counters == null || end == null) { missingEvidence = true; end = counters; return; }
        long produced = counters.producedFrames() - end.producedFrames();
        if (produced == 1) newFrameSamples++;
        else if (produced == 0) missingFrameSamples++;
        else { multipleFrameSamples++; if (produced < 0) missingEvidence = true; }
        end = counters;
    }

    boolean eligible(long expectedSamples) {
        if (missingEvidence || start == null || end == null || samples != expectedSamples || samples == 0) return false;
        return start.session() == end.session() && newFrameSamples == samples && end.producedFrames() - start.producedFrames() == samples
                && end.renderAttempts() - start.renderAttempts() == samples
                && end.pendingFrames() == start.pendingFrames() && end.unreadyFrames() == start.unreadyFrames()
                && end.duplicateFrames() == start.duplicateFrames() && end.failedFrames() == start.failedFrames();
    }

    JsonObject json(long expectedSamples) {
        JsonObject result = new JsonObject();
        result.addProperty("schema", 1);
        result.addProperty("source", "D3D12 new ready-fence publications");
        result.addProperty("complete", !missingEvidence && start != null && end != null && samples == expectedSamples);
        result.addProperty("capturedSamples", samples);
        result.addProperty("samplesWithNewFrame", newFrameSamples);
        result.addProperty("samplesWithoutNewFrame", missingFrameSamples);
        result.addProperty("samplesWithMultipleFrames", multipleFrameSamples);
        result.addProperty("eligibleForRenderedFps", eligible(expectedSamples));
        if (start != null) result.add("start", snapshotJson(start));
        if (end != null) result.add("end", snapshotJson(end));
        if (start != null && end != null) {
            JsonObject delta = new JsonObject();
            delta.addProperty("renderAttempts", end.renderAttempts() - start.renderAttempts());
            delta.addProperty("producedFrames", end.producedFrames() - start.producedFrames());
            delta.addProperty("pendingFrames", end.pendingFrames() - start.pendingFrames());
            delta.addProperty("unreadyFrames", end.unreadyFrames() - start.unreadyFrames());
            delta.addProperty("duplicateFrames", end.duplicateFrames() - start.duplicateFrames());
            delta.addProperty("failedFrames", end.failedFrames() - start.failedFrames());
            result.add("delta", delta);
        }
        return result;
    }

    static JsonObject snapshotJson(NativeFrameCounters.Snapshot counters) {
        JsonObject json = new JsonObject();
        json.addProperty("renderAttempts", counters.renderAttempts());
        json.addProperty("producedFrames", counters.producedFrames());
        json.addProperty("pendingFrames", counters.pendingFrames());
        json.addProperty("unreadyFrames", counters.unreadyFrames());
        json.addProperty("duplicateFrames", counters.duplicateFrames());
        json.addProperty("failedFrames", counters.failedFrames());
        json.addProperty("lastGeneration", counters.lastGeneration());
        json.addProperty("lastReadyFenceValue", counters.lastReadyFenceValue());
        json.addProperty("session", counters.session());
        return json;
    }
}
