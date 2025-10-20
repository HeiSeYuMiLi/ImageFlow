#pragma once

#include <chrono>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <source_location>
#include <string_view>

enum class LogLevel
{
    DEBUG,
    INFO,
    WARNING,
    ERROR,
    FATAL
};

class Logger
{
private:
    std::mutex mMutex;
    bool mConsoleOutput = true;
    LogLevel mCurrentLevel = LogLevel::INFO;
    std::optional<std::ofstream> mFileStream;

private:
    Logger() = default;

public:
    // 删除拷贝操作
    Logger(Logger const &) = delete;
    Logger &operator=(Logger const &) = delete;

public:
    static Logger &getInstance();

    // 设置日志级别
    void setLevel(LogLevel level);

    // 设置输出文件
    void setFile(std::string_view filename);

    // 设置控制台输出
    void setConsoleOutput(bool enable);

    // 格式化日志输出
    template <typename... Args>
    void debug(std::source_location const &location,
               std::format_string<Args...> fmt, Args &&...args)
    {
        log(LogLevel::DEBUG, location, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(std::source_location const &location,
              std::format_string<Args...> fmt, Args &&...args)
    {
        log(LogLevel::INFO, location, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warning(std::source_location const &location,
                 std::format_string<Args...> fmt, Args &&...args)
    {
        log(LogLevel::WARNING, location, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(std::source_location const &location,
               std::format_string<Args...> fmt, Args &&...args)
    {
        log(LogLevel::ERROR, location, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void fatal(std::source_location const &location,
               std::format_string<Args...> fmt, Args &&...args)
    {
        log(LogLevel::FATAL, location, fmt, std::forward<Args>(args)...);
    }

private:
    template <typename... Args>
    void log(LogLevel level, const std::source_location &location,
             std::format_string<Args...> fmt, Args &&...args)
    {
        if (level < mCurrentLevel)
            return;

        std::lock_guard _(mMutex);

        auto message = std::format(fmt, std::forward<Args>(args)...);

        std::string_view filename = location.file_name();
        if (auto pos = filename.find_last_of("/\\"); pos != std::string_view::npos)
            filename = filename.substr(pos + 1);
        auto logEntry = std::format("{} [{}] {} ({}:{})",
                                    getCurrentTime(),
                                    getLevelString(level),
                                    message,
                                    filename,         // 使用 string_view
                                    location.line()); // 使用 int
        outputLogEntry(level, logEntry);
    }

    void outputLogEntry(LogLevel level, std::string_view logEntry);

    std::string getLevelString(LogLevel level);

    std::string getCurrentTime();
};

#define LOG_DEBUG(...)   Logger::getInstance().debug(std::source_location::current(), __VA_ARGS__)
#define LOG_INFO(...)    Logger::getInstance().info(std::source_location::current(), __VA_ARGS__)
#define LOG_WARNING(...) Logger::getInstance().warning(std::source_location::current(), __VA_ARGS__)
#define LOG_ERROR(...)   Logger::getInstance().error(std::source_location::current(), __VA_ARGS__)
#define LOG_FATAL(...)   Logger::getInstance().fatal(std::source_location::current(), __VA_ARGS__)
