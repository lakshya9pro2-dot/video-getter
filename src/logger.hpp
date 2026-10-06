#pragma once

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace app_log {

enum class Level { Error = 0, Warn = 1, Info = 2, Debug = 3 };

inline Level configured_level() {
    static const Level level = [] {
        const char* value = std::getenv("LOG_LEVEL");
        if (!value) return Level::Info;
        std::string v(value);
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (v == "debug" || v == "trace") return Level::Debug;
        if (v == "warn" || v == "warning") return Level::Warn;
        if (v == "error") return Level::Error;
        return Level::Info;
    }();
    return level;
}

inline const char* name(Level level) {
    switch (level) {
        case Level::Error: return "ERROR";
        case Level::Warn: return "WARN";
        case Level::Debug: return "DEBUG";
        default: return "INFO";
    }
}

inline void write(Level level, const std::string& message) {
    if (static_cast<int>(level) > static_cast<int>(configured_level())) return;

    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);

    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
    gmtime_r(&tt, &tm);

    std::cerr << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
              << '.' << std::setfill('0') << std::setw(3) << ms.count()
              << "Z [" << name(level) << "] [tid="
              << std::hash<std::thread::id>{}(std::this_thread::get_id())
              << "] " << message << std::endl;
}

inline void info(const std::string& message) { write(Level::Info, message); }
inline void warn(const std::string& message) { write(Level::Warn, message); }
inline void error(const std::string& message) { write(Level::Error, message); }
inline void debug(const std::string& message) { write(Level::Debug, message); }

} // namespace app_log
