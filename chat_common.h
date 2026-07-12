#ifndef CHAT_COMMON_H
#define CHAT_COMMON_H

#include <algorithm>
#include <cctype>
#include <string>

inline std::string trim_copy(const std::string& input) {
    auto begin = std::find_if_not(input.begin(), input.end(), [](unsigned char ch) {
        return std::isspace(ch);
    });
    auto end = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char ch) {
        return std::isspace(ch);
    }).base();

    if (begin >= end) {
        return "";
    }
    return std::string(begin, end);
}

inline std::string format_chat_line(const std::string& room,
                                    const std::string& nickname,
                                    const std::string& message) {
    return "[" + room + "] " + nickname + ": " + message + "\n";
}

inline std::string format_notice_line(const std::string& message) {
    return "[알림] " + message + "\n";
}

#endif
