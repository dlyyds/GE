#include <Core/Log.h>
#include <Utils/PlatformUtils.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#ifdef GE_PLATFORM_ANDROID
#include <spdlog/sinks/android_sink.h>
#endif

#include <filesystem>
#include <vector>

namespace GE {

std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
std::shared_ptr<spdlog::logger> Log::s_ClientLogger;

namespace {

/**
 * @brief 尝试建立落盘的文件 sink；任何失败都返回 nullptr 而不是抛出。
 *
 * spdlog 的 basic_file_sink 在打不开目标时抛 `spdlog_ex`。Log::Init 是启动的
 * 第一步，把异常放出去就是**启动即崩**——Android 上这正是阶段 B 遗留的那道坎
 * （进程 CWD 是 `/`，不可写）。日志本身不是启动的必要条件，所以退化为
 * "只有控制台/logcat sink" 即可。
 */
spdlog::sink_ptr TryMakeFileSink(const std::filesystem::path &dir, std::string &outErr) {
    if (dir.empty()) {
        outErr = "无法定位可写用户目录";
        return nullptr;
    }
    try {
        return std::make_shared<spdlog::sinks::basic_file_sink_mt>((dir / "GE.log").string(), true);
    } catch (const spdlog::spdlog_ex &e) {
        outErr = e.what();
        return nullptr;
    }
}

} // namespace

void Log::Init() {

    GE_PROFILE_FUNCTION();

    // 刻意用具名指针而非 logSinks[0]/[1] 设 pattern：sink 数量随平台变化
    // （Android 多一个 logcat sink），按下标取会静默错位。
    std::vector<spdlog::sink_ptr> logSinks;

    // %^ %$ 颜色范围，%T 时间，%n 日志器名，%s:%# 文件:行号，%v 消息内容
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink->set_pattern("%^[%T] %n [%s:%#] %v%$");
    logSinks.emplace_back(std::move(consoleSink));

#ifdef GE_PLATFORM_ANDROID
    // Android 上 stdout 不接终端——不看 logcat 就等于没有控制台输出。
    // tag 用 "GE"，便于 `adb logcat -s GE` 直接过滤。
    auto androidSink = std::make_shared<spdlog::sinks::android_sink_mt>("GE");
    androidSink->set_pattern("[%T] [%l] %n [%s:%#] %v");
    logSinks.emplace_back(std::move(androidSink));
#endif

    std::string fileSinkError;
    const std::filesystem::path userDir = PlatformUtils::GetUserDataDirectory();
    if (spdlog::sink_ptr fileSink = TryMakeFileSink(userDir, fileSinkError)) {
        fileSink->set_pattern("[%T] [%l] %n [%s:%#] %v");
        logSinks.emplace_back(std::move(fileSink));
    }

    s_CoreLogger = std::make_shared<spdlog::logger>("GE", begin(logSinks), end(logSinks));
    spdlog::register_logger(s_CoreLogger);
    s_CoreLogger->set_level(spdlog::level::trace);
    s_CoreLogger->flush_on(spdlog::level::trace);

    s_ClientLogger = std::make_shared<spdlog::logger>("CLIENT", begin(logSinks), end(logSinks));
    spdlog::register_logger(s_ClientLogger);
    s_ClientLogger->set_level(spdlog::level::trace);
    s_ClientLogger->flush_on(spdlog::level::trace);

    // 告警必须放在 logger 建好之后 —— 这之前 GE_CORE_WARN 会解引用空 logger
    if (!fileSinkError.empty()) {
        GE_CORE_WARN("Log: 文件日志不可用（{0}），仅输出到控制台", fileSinkError);
    } else {
        GE_CORE_INFO("Log: 文件日志 → {0}", (userDir / "GE.log").string());
    }
}

} // namespace GE
