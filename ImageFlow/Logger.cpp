#include "Logger.hpp"

Logger &Logger::getInstance()
{
    static Logger instance;
    return instance;
}

void Logger::setLevel(LogLevel level)
{
    std::lock_guard _(mMutex);
    mCurrentLevel = level;
}

void Logger::setFile(std::string_view filename)
{
    std::lock_guard _(mMutex);
    mFileStream.emplace(filename.data(), std::ios::app);
    if (!mFileStream->is_open())
    {
        std::cerr << std::format("����־�ļ�ʧ�ܣ�{}\n", filename);
        mFileStream.reset();
    }
}

void Logger::setConsoleOutput(bool enable)
{
    std::lock_guard _(mMutex);
    mConsoleOutput = enable;
}

void Logger::outputLogEntry(LogLevel level, std::string_view logEntry)
{
    // ���������̨
    if (mConsoleOutput)
    {
        if (level >= LogLevel::ERROR)
            std::cerr << logEntry << '\n';
        else
            std::cout << logEntry << '\n';
        // ǿ��ˢ�£�ȷ����ʱ���
        std::cout.flush();
    }

    // ������ļ�
    if (mFileStream && mFileStream->is_open())
    {
        *mFileStream << logEntry << '\n';
        mFileStream->flush();
    }
}

std::string Logger::getLevelString(LogLevel level)
{
    using enum LogLevel;
    switch (level)
    {
    case DEBUG:
        return "DEBUG";
    case INFO:
        return "INFO";
    case WARNING:
        return "WARNING";
    case ERROR:
        return "ERROR";
    case FATAL:
        return "FATAL";
    default:
        return "UNKNOWN";
    }
}

std::string Logger::getCurrentTime()
{
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) %
              1000;

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &time_t);
#else
    localtime_r(&time_t, &tm);
#endif

    // �ֶ�����ʱ���ַ�������������ʽ������
    char time_buffer[64];
    std::strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return std::format("{}.{:03d}", time_buffer, ms.count());
}