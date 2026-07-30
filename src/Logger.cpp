#include "Logger.h"
#include <chrono>
#include <iomanip>
#include <sstream>
#include <shlobj.h>

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fileStream.is_open()) {
        m_fileStream.close();
    }
}

void Logger::Init(const std::wstring& logFilePath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) return;

    m_fileStream.open(logFilePath, std::ios::out | std::ios::app);
    m_initialized = true;
}

void Logger::Log(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    struct tm tm_now = {};
    localtime_s(&tm_now, &time_t_now);

    const char* levelStr = "[INFO]";
    if (level == LogLevel::Warning) levelStr = "[WARN]";
    else if (level == LogLevel::Error) levelStr = "[ERR ]";

    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tm_now);

    std::string formatted = std::string(timeBuf) + " " + levelStr + " " + message + "\n";

    OutputDebugStringA(formatted.c_str());

    if (m_fileStream.is_open()) {
        m_fileStream << formatted;
        m_fileStream.flush();
    }
}

void Logger::LogW(LogLevel level, const std::wstring& message) {
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, message.c_str(), (int)message.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, message.c_str(), (int)message.size(), &strTo[0], size_needed, NULL, NULL);
    Log(level, strTo);
}
