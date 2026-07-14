#include "diagnostic_log.hpp"

#include <obs-module.h>

#include <util/bmem.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <system_error>
#include <utility>

namespace alpha_recorder::obs
{
    namespace
    {
        std::mutex g_diagnostic_log_mutex;

        bool set_error(std::string *error_message, std::string message)
        {
            if (error_message != nullptr)
            {
                *error_message = std::move(message);
            }

            return false;
        }

        std::filesystem::path path_from_utf8(const char *text)
        {
            if (text == nullptr || *text == '\0')
            {
                return {};
            }

            return std::filesystem::u8path(text);
        }

        std::string timestamp_text()
        {
            const auto now = std::chrono::system_clock::now();
            const std::time_t time = std::chrono::system_clock::to_time_t(now);
            std::tm local_time{};
#ifdef _WIN32
            (void)localtime_s(&local_time, &time);
#else
            (void)localtime_r(&time, &local_time);
#endif

            std::ostringstream stream;
            stream << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");
            return stream.str();
        }
    } // namespace

    std::filesystem::path diagnostic_log_path() noexcept
    {
        char *path = obs_module_config_path("diagnostics/alpha-recorder.log");
        std::filesystem::path result = path_from_utf8(path);
        if (path != nullptr)
        {
            bfree(path);
        }

        return result;
    }

    bool ensure_diagnostic_log_file(std::string *error_message) noexcept
    {
        try
        {
            const std::filesystem::path path = diagnostic_log_path();
            if (path.empty())
            {
                return set_error(error_message, "Alpha Recorder diagnostic log path is unavailable.");
            }

            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
            {
                return set_error(error_message, std::string{"Alpha Recorder could not create the diagnostic log directory: "} +
                                                    error.message());
            }

            std::ofstream stream(path, std::ios::app);
            if (!stream)
            {
                return set_error(error_message, "Alpha Recorder could not open the diagnostic log file.");
            }

            return true;
        }
        catch (const std::exception &ex)
        {
            return set_error(error_message, std::string{"Alpha Recorder could not prepare the diagnostic log file: "} + ex.what());
        }
        catch (...)
        {
            return set_error(error_message, "Alpha Recorder could not prepare the diagnostic log file.");
        }
    }

    void append_diagnostic_log_line(std::string_view line) noexcept
    {
        std::lock_guard<std::mutex> lock(g_diagnostic_log_mutex);
        std::string error_message;
        if (!ensure_diagnostic_log_file(&error_message))
        {
            blog(LOG_WARNING, "%s", error_message.c_str());
            return;
        }

        std::ofstream stream(diagnostic_log_path(), std::ios::app);
        if (!stream)
        {
            blog(LOG_WARNING, "Alpha Recorder could not append to the diagnostic log file.");
            return;
        }

        stream << '[' << timestamp_text() << "] " << line << '\n';
    }

} // namespace alpha_recorder::obs
