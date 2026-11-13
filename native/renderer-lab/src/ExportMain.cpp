/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 */
#include <renderer_lab/GltfExport.h>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc<3 || argc>4) throw std::runtime_error("Usage: RendererLabExport input.rslb output-directory [--allow-approximations]");
        renderer_lab::ExportOptions options;
        if (argc==4) {
            if (std::string(argv[3])!="--allow-approximations") throw std::runtime_error("Unknown export option");
            options.allowApproximations=true;
        }
        renderer_lab::SceneState scene; scene.load(argv[1]);
        auto exported=renderer_lab::exportGltf(scene,argv[2],options);
        std::cout << exported.sceneFile.string() << '\n' << exported.instances << " instances, " << exported.triangles << " triangles\n";
        for (const auto& warning:exported.limitations) std::cerr << "APPROXIMATION: " << warning << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << "Scene export failed: " << e.what() << '\n'; return 1; }
}
