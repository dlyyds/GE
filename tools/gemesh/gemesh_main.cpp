/**
 * @file gemesh_main.cpp
 * @brief .gemesh 离线导出 CLI 工具：把源模型（.obj / .gltf）烘焙成引擎内置二进制格式。
 *
 * 纯 CPU 工具，不初始化 Vulkan/窗口——直接调用 GE::ModelLoader::ConvertToGEMesh
 * 复用引擎同一套「解析 + 切线计算 + 包围盒 + 内嵌路径归一 + 序列化」逻辑，保证 CLI
 * 产物与游戏内 LoadMesh(.gemesh) 加载语义完全一致。
 *
 * 用法：
 *   gemesh <src.obj> [out.gemesh]        输出缺省 = src 同目录同名 .gemesh
 *   gemesh -i <src.obj> -o <out.gemesh>
 *   gemesh -i <src.gltf> -n 2 -o <out.gemesh>     glTF 多 mesh 导出
 *   gemesh -h | --help
 *   gemesh -v | --version
 */

#include "Render/ModelLoader.h"
#include "Core/Log.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

void PrintUsage() {
    std::cout <<
        "用法: gemesh <src> [out.gemesh]\n"
        "      gemesh -i <src> -o <out.gemesh> [-n <mesh 索引>] [-r <资源根>]\n"
        "      gemesh -h | --help      显示帮助\n"
        "      gemesh -v | --version   显示版本\n\n"
        "把源模型（.obj / .gltf / .glb）离线烘焙为 .gemesh 引擎内置格式。\n"
        "不指定输出路径时，输出到源文件同目录同名 .gemesh（多 mesh 加 _N 后缀）。\n\n"
        "  -n, --mesh-index <N>   glTF 的 mesh 索引，默认 0（一个 glTF 可导出多个 .gemesh）\n"
        "  -r, --asset-root <dir> 资源根目录（默认 assets，相对当前工作目录）——\n"
        "                        用于把 .gemesh 内嵌的贴图路径压成相对资源根的规范形。\n"
        "                        .gemesh 不是自包含格式，内嵌绝对路径换台机器必断，\n"
        "                        故建议 src 传绝对路径、资源根传真实根目录。\n";
}

void PrintVersion() {
    std::cout << "gemesh 0.2.0（.gemesh 格式版本 2，Vertex 80B 含蒙皮字段；内嵌路径归一）\n";
}

} // namespace

int main(int argc, char **argv) {
    // 初始化 spdlog logger（GE_CORE_* 宏依赖），否则序列化收尾日志空指针崩溃
    GE::Log::Init();

    std::string src, out, assetRoot = "assets";
    size_t meshIndex = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            PrintUsage();
            return 0;
        }
        if (a == "-v" || a == "--version") {
            PrintVersion();
            return 0;
        }
        if (a == "-i" && i + 1 < argc) {
            src = argv[++i];
            continue;
        }
        if (a == "-o" && i + 1 < argc) {
            out = argv[++i];
            continue;
        }
        if ((a == "-n" || a == "--mesh-index") && i + 1 < argc) {
            meshIndex = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
            continue;
        }
        if ((a == "-r" || a == "--asset-root") && i + 1 < argc) {
            assetRoot = argv[++i];
            continue;
        }
        if (a.rfind("-", 0) == 0) {
            std::cerr << "[gemesh] 未知参数: " << a << "\n";
            PrintUsage();
            return 1;
        }
        // 位置参数：第一个为 src，第二个为 out
        if (src.empty()) {
            src = a;
        } else if (out.empty()) {
            out = a;
        } else {
            std::cerr << "[gemesh] 参数过多\n";
            PrintUsage();
            return 1;
        }
    }

    if (src.empty()) {
        PrintUsage();
        return 1;
    }

    // 未指定输出 → 源文件同目录同名 .gemesh（多 mesh 加 _N，与 SceneSerializer
    // 的 DeriveGemeshOutPath 同一命名约定，场景里的 "foo.gltf#N" 靠它对应上）
    if (out.empty()) {
        fs::path p(src);
        const std::string stem = meshIndex > 0
            ? p.stem().string() + "_" + std::to_string(meshIndex)
            : p.stem().string();
        out = (p.parent_path() / (stem + ".gemesh")).string();
    }

    std::string err;
    if (!GE::ModelLoader::ConvertToGEMesh(src, out, &err, assetRoot, meshIndex)) {
        std::cerr << "[gemesh] 转换失败: " << err << "\n";
        return 1;
    }

    std::cout << "[gemesh] 完成: " << src << " -> " << out << "\n";
    return 0;
}
