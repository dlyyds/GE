#include <GE.h>
#include <GE/Core/EntryPoint.h>

namespace GE {
/// 最小沙盒应用：仅运行引擎主循环（ImGui），作为引擎冒烟测试。
/// 编辑器功能已拆分到独立的 GE_Editor 可执行。
class Sandbox : public Application {
public:
    explicit Sandbox(ApplicationCommandLineArgs args) : Application("Sandbox", args) {
        GE_PROFILE_FUNCTION();
        // 不推任何自定义 Layer，仅使用引擎默认的 ImGuiLayer？
    }

    ~Sandbox() override = default;
};

Application *CreateApplication(const ApplicationCommandLineArgs args) { return new Sandbox(args); }

} // namespace GE