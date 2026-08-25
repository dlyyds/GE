/**
 * @file gemesh_main.cpp
 * @brief .gemesh 离线导出 CLI 工具：把源模型（.obj）烘焙成引擎内置二进制格式。
 *
 * 纯 CPU 工具，不初始化 Vulkan/窗口——直接调用 GE::ModelLoader::ConvertToGEMesh
 * 复用引擎同一套「OBJ 解析 + 切线计算 + 包围盒 + 序列化」逻辑，保证 CLI 产物与
 * 游戏内 LoadMesh(.gemesh) 加载语义完全一致。
 *
 * 用法：
 *   gemesh <src.obj> [out.gemesh]        输出缺省 = src 同目录同名 .gemesh
 *   gemesh -i <src.obj> -o <out.gemesh>
 *   gemesh -h | --help
 *   gemesh -v | --version
 */

#include "Render/ModelLoader.h"
#include "Core/Log.h"

#include <iostream>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

void PrintUsage() {
    std::cout <<
        "用法: gemesh <src.obj> [out.gemesh]\n"
        "      gemesh -i <src.obj> -o <out.gemesh>\n"
        "      gemesh -h | --help      显示帮助\n"
        "      gemesh -v | --version   显示版本\n\n"
        "把源模型（当前支持 .obj）离线烘焙为 .gemesh 引擎内置格式。\n"
        "不指定输出路径时，输出到源文件同目录同名 .gemesh。\n";
}

void PrintVersion() {
    std::cout << "gemesh 0.1.1（.gemesh 格式版本 2，Vertex 80B 含蒙皮字段）\n";
}

} // namespace

int main(int argc, char **argv) {
    // 初始化 spdlog logger（GE_CORE_* 宏依赖），否则序列化收尾日志空指针崩溃
    GE::Log::Init();

    std::string src, out;

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

    // 未指定输出 → 源文件同目录同名 .gemesh
    if (out.empty()) {
        out = fs::path(src).replace_extension(".gemesh").string();
    }

    std::string err;
    if (!GE::ModelLoader::ConvertToGEMesh(src, out, &err)) {
        std::cerr << "[gemesh] 转换失败: " << err << "\n";
        return 1;
    }

    std::cout << "[gemesh] 完成: " << src << " -> " << out << "\n";
    return 0;
}
