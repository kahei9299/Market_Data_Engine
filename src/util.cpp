#include "util.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>

std::string timestamp_now() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) %
              1000;
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0')
        << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}
