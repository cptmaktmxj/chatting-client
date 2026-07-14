#ifndef CHAT_COMMON_H
#define CHAT_COMMON_H

#include <algorithm>
#include <cctype>
#include <string>

// 문자열 양끝의 공백 문자를 제거한 복사본을 반환합니다.
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

// 방 이름, 닉네임, 메시지를 채팅 출력 형식으로 조합합니다.
inline std::string format_chat_line(const std::string& room,
                                    const std::string& nickname,
                                    const std::string& message) {
    return "[" + room + "] " + nickname + ": " + message + "\n";
}

// 시스템 알림 메시지를 표준 알림 출력 형식으로 조합합니다.
inline std::string format_notice_line(const std::string& message) {
    return "[알림] " + message + "\n";
}

#endif
