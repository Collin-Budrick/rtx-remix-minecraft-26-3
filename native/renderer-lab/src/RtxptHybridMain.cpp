/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 * Integrates NVIDIA RTXPT through its original implementation, under the
 * unmodified NVIDIA license preserved in licenses/RTXPT-LICENSE.txt.
 */
#define WinMain RendererLabOriginalSdkWinMain
#include <IntroSample/IntroSample.cpp>
#undef WinMain
#define RENDERER_LAB_SDK_RENDERER IntroPathTracer
#define RENDERER_LAB_MODE_NAME "rtxpt-raster-primary"
#define RENDERER_LAB_HYBRID 1
#include "RtxptEntry.h"
