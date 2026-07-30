#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <mutex>
#include <fstream>
#include <windows.h>

enum class LogLevel {
    Info,
    Warning,
    Error
};

class Logger {
public:
    static Logger& Instance();

    void Init(const std::wstring& logFilePath);
    void Log(LogLevel level, const std::string& message);
    void LogW(LogLevel level, const std::wstring& message);

private:
    Logger() = default;
    ~Logger();

    std::mutex m_mutex;
    std::ofstream m_fileStream;
    bool m_initialized = false;
};

#define LOG_INFO(msg) Logger::Instance().Log(LogLevel::Info, msg)
#define LOG_WARN(msg) Logger::Instance().Log(LogLevel::Warning, msg)
#define LOG_ERROR(msg) Logger::Instance().Log(LogLevel::Error, msg)

#define LOG_INFOW(msg) Logger::Instance().LogW(LogLevel::Info, msg)
#define LOG_WARNW(msg) Logger::Instance().LogW(LogLevel::Warning, msg)
#define LOG_ERRORW(msg) Logger::Instance().LogW(LogLevel::Error, msg)

#endif // LOGGER_H
