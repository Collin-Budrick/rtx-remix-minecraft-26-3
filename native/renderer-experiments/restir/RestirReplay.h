/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <renderer_lab/ScenePacket.h>
#include <filesystem>
#include <memory>
#include <string>
#include <cstdint>

class Profiler;
struct UIData;
namespace nvrhi { class IDevice; class ITexture; }
namespace donut::app { class DeviceManager; }

namespace renderer_lab {
struct RestirReplayConfig {
    SceneState scene;
    std::filesystem::path directory;
    std::string environmentPath;
    std::uint32_t warmupFrames=120,recordFrames=120;
    bool requireRayReconstruction=true;
    bool completed{};
};
RestirReplayConfig& replayConfig();
void recordGpuFrame(UIData& ui,const std::shared_ptr<Profiler>& profiler,nvrhi::IDevice* device,
                    nvrhi::ITexture* color,std::uint32_t submittedFrame,donut::app::DeviceManager* manager);
} // namespace renderer_lab
