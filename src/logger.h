#pragma once

#include <spdlog/sinks/basic_file_sink.h>

namespace logger = SKSE::log;

inline void SetupLog()
{
    const auto logsFolder = SKSE::log::log_directory();
    if (!logsFolder) {
        SKSE::stl::report_and_fail("SKSE log directory is unavailable");
    }

    const auto pluginName = SKSE::PluginDeclaration::GetSingleton()->GetName();
    const auto logPath = *logsFolder / std::format("{}.log", pluginName);
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
    auto log = std::make_shared<spdlog::logger>("TileableFavoritesMenu", std::move(sink));
    spdlog::set_default_logger(std::move(log));
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
}
