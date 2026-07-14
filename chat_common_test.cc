#include "chat_common.h"

#include <cassert>
#include <string>

// 공통 문자열 유틸리티가 기대한 값을 반환하는지 검증하는 테스트 진입점입니다.
int main() {
    assert(trim_copy("  hello  ") == "hello");
    assert(trim_copy("\n\t/quit\r\n") == "/quit");
    assert(format_chat_line("room1", "kim", "안녕하세요") == "[room1] kim: 안녕하세요\n");
    assert(format_notice_line("kim joined") == "[알림] kim joined\n");
    return 0;
}
