#ifndef OKJFMT_H
#define OKJFMT_H

#include <string>
#include <QString>
#include <spdlog/fmt/fmt.h>

// fmt 10+ (bundled with spdlog 1.13+) no longer formats types implicitly via a
// visible std::ostream operator<<, which the logging calls in this codebase
// relied on for QString arguments. Provide an explicit formatter instead.
template<>
struct fmt::formatter<QString> : fmt::formatter<std::string> {
    template<typename FormatContext>
    auto format(const QString &s, FormatContext &ctx) const {
        return fmt::formatter<std::string>::format(s.toStdString(), ctx);
    }
};

#endif // OKJFMT_H
