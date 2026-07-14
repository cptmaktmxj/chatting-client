#include "chat_common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#define BUFSIZE 1024
#define MAX_CLIENT 64
#define DEFAULT_HISTORY_LIMIT 20
#define MAX_HISTORY_LIMIT 50

enum RoomType {
    ROOM_NORMAL,
    ROOM_HIDDEN,
    ROOM_ANNOUNCE,
    ROOM_APPROVAL,
    ROOM_SECRET,
    ROOM_PRIVATE
};

struct Room {
    std::string name;
    RoomType type;
    std::string owner;
    std::string password_hash;
    std::set<std::string> admins;
    std::set<std::string> permitted;
    std::set<std::string> banned;
    std::set<std::string> speak_requests;
    std::map<std::string, std::string> anonymous_names;
    int next_anonymous_id;
};

struct Client {
    int sock;
    int id;
    std::string username;
    std::string nickname;
    std::string profile;
    std::string status;
    bool profile_public;
    bool status_public;
    std::string room;
    std::set<std::string> muted_rooms;
    std::vector<std::string> mentions;
    std::time_t last_active;
};

struct MessageRecord {
    int id;
    std::string room;
    std::string owner;
    RoomType room_type;
    std::string sender;
    std::string shown_sender;
    std::string text;
    std::string time_text;
    int reply_to;
    bool edited;
    bool deleted;
    bool pinned;
    std::string pinned_by;
};

struct Account {
    std::string username;
    std::string password_hash;
    std::string nickname;
    std::string profile;
    std::string status;
    bool profile_public;
    bool status_public;
    std::time_t last_seen;
};

std::vector<Client*> clients;
std::map<std::string, Room> rooms;
std::map<std::string, std::string> open_links;
std::map<std::string, std::string> open_link_passwords;
std::map<std::string, std::set<std::string> > open_link_banned;
std::map<std::string, std::string> private_room_links;
std::map<std::string, std::map<std::string, std::string> > templates;
std::map<std::string, Account> accounts;
std::map<int, MessageRecord> messages;
std::vector<int> message_order;
std::map<int, std::map<std::string, std::string> > reactions;
std::map<std::string, std::vector<std::string> > mod_logs;
pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
int next_client_id = 1;
int next_message_id = 1;
std::string state_path = "chat_state.db";

// 치명적인 서버 오류를 출력하고 프로세스를 종료합니다.
void error_handling(const char* message) {
    perror(message);
    exit(1);
}

// 지정한 구분자로 문자열을 나누어 토큰 목록을 만듭니다.
std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> out;
    std::string item;
    std::istringstream iss(text);
    while (std::getline(iss, item, delimiter)) out.push_back(item);
    return out;
}

// 첫 번째 공백 전 토큰과 나머지 문자열을 분리합니다.
std::string first_token(const std::string& text, std::string* rest) {
    std::string trimmed = trim_copy(text);
    size_t pos = trimmed.find(' ');
    if (pos == std::string::npos) {
        if (rest != NULL) *rest = "";
        return trimmed;
    }
    if (rest != NULL) *rest = trim_copy(trimmed.substr(pos + 1));
    return trimmed.substr(0, pos);
}

// 문자열 집합을 콤마로 구분된 저장용 문자열로 변환합니다.
std::string join_set(const std::set<std::string>& values) {
    std::ostringstream oss;
    bool first = true;
    for (const std::string& value : values) {
        if (!first) oss << ",";
        oss << value;
        first = false;
    }
    return oss.str();
}

// 콤마로 구분된 문자열을 문자열 집합으로 복원합니다.
std::set<std::string> parse_set(const std::string& text) {
    std::set<std::string> values;
    for (const std::string& item : split(text, ',')) {
        std::string value = trim_copy(item);
        if (!value.empty()) values.insert(value);
    }
    return values;
}

// 상태 파일에 안전하게 저장하도록 특수 문자를 퍼센트 인코딩합니다.
std::string escape_field(const std::string& text) {
    std::ostringstream oss;
    for (unsigned char ch : text) {
        if (ch == '%' || ch == '|' || ch == '\n' || ch == '\r') {
            oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(ch) << std::nouppercase << std::dec;
        } else {
            oss << ch;
        }
    }
    return oss.str();
}

// 16진수 문자 하나를 정수 값으로 변환합니다.
int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

// 퍼센트 인코딩된 상태 파일 필드를 원래 문자열로 복원합니다.
std::string unescape_field(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '%' && i + 2 < text.size()) {
            int high = hex_value(text[i + 1]);
            int low = hex_value(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>(high * 16 + low));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

// 방 타입 열거값을 상태 파일과 출력에 쓰는 문자열로 변환합니다.
std::string room_type_to_string(RoomType type) {
    switch (type) {
        case ROOM_HIDDEN: return "hidden";
        case ROOM_ANNOUNCE: return "announce";
        case ROOM_APPROVAL: return "approval";
        case ROOM_SECRET: return "secret";
        case ROOM_PRIVATE: return "private";
        case ROOM_NORMAL:
        default: return "normal";
    }
}

// 저장된 방 타입 문자열을 방 타입 열거값으로 변환합니다.
RoomType parse_room_type(const std::string& type) {
    if (type == "hidden") return ROOM_HIDDEN;
    if (type == "announce") return ROOM_ANNOUNCE;
    if (type == "approval") return ROOM_APPROVAL;
    if (type == "secret" || type == "secure") return ROOM_SECRET;
    if (type == "private") return ROOM_PRIVATE;
    return ROOM_NORMAL;
}

// 비밀방과 계정 비밀번호를 직접 저장하지 않도록 해시 문자열을 만듭니다.
std::string hash_password(const std::string& password) {
    std::hash<std::string> hasher;
    size_t value = hasher("chat-password:" + password);
    std::ostringstream oss;
    oss << std::hex << value;
    return oss.str();
}

// 채팅 메시지에 표시할 현재 시각을 HH:MM 형식으로 반환합니다.
std::string current_time_text() {
    std::time_t now = std::time(NULL);
    struct tm local_tm;
    localtime_r(&now, &local_tm);
    char buffer[6];
    std::strftime(buffer, sizeof(buffer), "%H:%M", &local_tm);
    return std::string(buffer);
}

// 공개 설정 명령의 on/off 값을 bool 값으로 해석합니다.
bool parse_on_off(const std::string& value, bool* out) {
    std::string lowered = trim_copy(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return std::tolower(ch);
    });
    if (lowered == "on" || lowered == "public") {
        *out = true;
        return true;
    }
    if (lowered == "off" || lowered == "private") {
        *out = false;
        return true;
    }
    return false;
}

// 숫자 인자를 범위 안의 히스토리 개수로 변환합니다.
int parse_history_limit(const std::string& text) {
    int value = atoi(trim_copy(text).c_str());
    if (value <= 0) return DEFAULT_HISTORY_LIMIT;
    if (value > MAX_HISTORY_LIMIT) return MAX_HISTORY_LIMIT;
    return value;
}

// 버퍼 전체가 소켓에 기록될 때까지 반복 전송합니다.
bool send_all(int sock, const std::string& data) {
    const char* buf = data.c_str();
    size_t total = 0;
    while (total < data.size()) {
        ssize_t sent = write(sock, buf + total, data.size() - total);
        if (sent <= 0) return false;
        total += static_cast<size_t>(sent);
    }
    return true;
}

// 특정 클라이언트에게 메시지를 전송합니다.
void send_to_client(Client* client, const std::string& message) {
    if (client != NULL) send_all(client->sock, message);
}

// 새 방의 기본 권한과 소유자 정보를 초기화합니다.
Room make_room(const std::string& name, RoomType type, const std::string& owner, const std::string& password = "") {
    Room room;
    room.name = name;
    room.type = type;
    room.owner = owner;
    room.password_hash = password.empty() ? "" : hash_password(password);
    room.admins.insert(owner);
    room.permitted.insert(owner);
    room.next_anonymous_id = 1;
    return room;
}

// 운영 로그를 메모리와 상태 파일에 남길 수 있도록 방별 목록에 추가합니다.
void add_mod_log_unlocked(const std::string& room_name, const std::string& text) {
    std::vector<std::string>& log = mod_logs[room_name];
    log.push_back(current_time_text() + " " + text);
    if (log.size() > 50) log.erase(log.begin());
}

// 잠금이 잡힌 상태에서 방, 링크, 계정, 템플릿 정보를 상태 파일에 저장합니다.
void save_state_unlocked() {
    std::ofstream out(state_path.c_str(), std::ios::trunc);
    for (const auto& pair : rooms) {
        const Room& room = pair.second;
        out << "ROOM|" << escape_field(room.name) << "|" << room_type_to_string(room.type) << "|"
            << escape_field(room.owner) << "|" << room.password_hash << "|" << join_set(room.admins)
            << "|" << join_set(room.permitted) << "|" << join_set(room.banned)
            << "|" << join_set(room.speak_requests) << "\n";
    }
    for (const auto& pair : open_links) {
        out << "LINK|" << escape_field(pair.first) << "|" << escape_field(pair.second) << "\n";
    }
    for (const auto& pair : open_link_passwords) {
        out << "LINKPWD|" << escape_field(pair.first) << "|" << pair.second << "\n";
    }
    for (const auto& pair : open_link_banned) {
        out << "LINKBAN|" << escape_field(pair.first) << "|" << join_set(pair.second) << "\n";
    }
    for (const auto& pair : private_room_links) {
        out << "PRIVLINK|" << escape_field(pair.first) << "|" << escape_field(pair.second) << "\n";
    }
    for (const auto& owner_pair : templates) {
        for (const auto& template_pair : owner_pair.second) {
            out << "TEMPLATE|" << escape_field(owner_pair.first) << "|"
                << escape_field(template_pair.first) << "|"
                << escape_field(template_pair.second) << "\n";
        }
    }
    for (const auto& pair : accounts) {
        const Account& account = pair.second;
        out << "ACCOUNT|" << escape_field(account.username) << "|" << account.password_hash << "|"
            << escape_field(account.nickname) << "|" << escape_field(account.profile) << "|"
            << escape_field(account.status) << "|" << (account.profile_public ? "1" : "0") << "|"
            << (account.status_public ? "1" : "0") << "|" << static_cast<long long>(account.last_seen) << "\n";
    }
    for (const auto& pair : mod_logs) {
        for (const std::string& entry : pair.second) {
            out << "MODLOG|" << escape_field(pair.first) << "|" << escape_field(entry) << "\n";
        }
    }
}

// 상태 파일에서 방, 링크, 계정, 템플릿 정보를 읽고 기본 lobby 방을 보장합니다.
void load_state() {
    pthread_mutex_lock(&state_mutex);
    rooms.clear();
    open_links.clear();
    open_link_passwords.clear();
    open_link_banned.clear();
    private_room_links.clear();
    templates.clear();
    accounts.clear();
    mod_logs.clear();

    std::ifstream in(state_path.c_str());
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> parts = split(line, '|');
        if (parts.size() >= 7 && parts[0] == "ROOM") {
            Room room;
            room.name = unescape_field(parts[1]);
            room.type = parse_room_type(parts[2]);
            room.owner = unescape_field(parts[3]);
            room.password_hash = parts[4];
            room.admins = parse_set(parts[5]);
            room.permitted = parse_set(parts[6]);
            if (parts.size() >= 8) room.banned = parse_set(parts[7]);
            if (parts.size() >= 9) room.speak_requests = parse_set(parts[8]);
            if (room.admins.empty() && !room.owner.empty()) room.admins.insert(room.owner);
            room.next_anonymous_id = 1;
            rooms[room.name] = room;
        } else if (parts.size() >= 3 && parts[0] == "LINK") {
            open_links[unescape_field(parts[1])] = unescape_field(parts[2]);
        } else if (parts.size() >= 3 && parts[0] == "LINKPWD") {
            open_link_passwords[unescape_field(parts[1])] = parts[2];
        } else if (parts.size() >= 3 && parts[0] == "LINKBAN") {
            open_link_banned[unescape_field(parts[1])] = parse_set(parts[2]);
        } else if (parts.size() >= 3 && parts[0] == "PRIVLINK") {
            private_room_links[unescape_field(parts[1])] = unescape_field(parts[2]);
        } else if (parts.size() >= 4 && parts[0] == "TEMPLATE") {
            templates[unescape_field(parts[1])][unescape_field(parts[2])] = unescape_field(parts[3]);
        } else if (parts.size() >= 9 && parts[0] == "ACCOUNT") {
            Account account;
            account.username = unescape_field(parts[1]);
            account.password_hash = parts[2];
            account.nickname = unescape_field(parts[3]);
            account.profile = unescape_field(parts[4]);
            account.status = unescape_field(parts[5]);
            account.profile_public = parts[6] == "1";
            account.status_public = parts[7] == "1";
            account.last_seen = static_cast<std::time_t>(atoll(parts[8].c_str()));
            accounts[account.username] = account;
        } else if (parts.size() >= 3 && parts[0] == "MODLOG") {
            mod_logs[unescape_field(parts[1])].push_back(unescape_field(parts[2]));
        }
    }

    if (rooms.count("lobby") == 0) rooms["lobby"] = make_room("lobby", ROOM_NORMAL, "server");
    save_state_unlocked();
    pthread_mutex_unlock(&state_mutex);
}

// 잠금이 잡힌 상태에서 사용자가 방 관리자인지 확인합니다.
bool is_admin_unlocked(const Room& room, const std::string& nickname) {
    return room.admins.count(nickname) > 0;
}

// 잠금이 잡힌 상태에서 사용자가 방장인지 확인합니다.
bool is_owner_unlocked(const Room& room, const std::string& nickname) {
    return room.owner == nickname;
}

// 잠금이 잡힌 상태에서 사용자가 현재 방에 말할 권한이 있는지 확인합니다.
bool can_speak_unlocked(const Room& room, const std::string& nickname) {
    if (is_admin_unlocked(room, nickname)) return true;
    if (room.type == ROOM_ANNOUNCE) return false;
    if (room.type == ROOM_APPROVAL) return room.permitted.count(nickname) > 0;
    return true;
}

// 숨김 방에서는 방 안에서 안정적인 익명 번호를, 일반 방에서는 실제 닉네임을 반환합니다.
std::string display_name_unlocked(Room& room, const std::string& nickname) {
    if (room.type != ROOM_HIDDEN) return nickname;
    auto it = room.anonymous_names.find(nickname);
    if (it != room.anonymous_names.end()) return it->second;
    std::string anonymous = "익명" + std::to_string(room.next_anonymous_id++);
    room.anonymous_names[nickname] = anonymous;
    return anonymous;
}

// 잠금이 잡힌 상태에서 닉네임으로 접속 중인 클라이언트를 찾습니다.
Client* find_client_by_nick_unlocked(const std::string& nickname) {
    for (Client* client : clients) {
        if (client->nickname == nickname) return client;
    }
    return NULL;
}

// 공개 설정에 따라 보여줄 프로필 문자열을 결정합니다.
std::string visible_profile_for(const Client* client) {
    if (!client->profile_public) return "private";
    return client->profile.empty() ? "(empty)" : client->profile;
}

// 공개 설정과 자동 자리비움 기준에 따라 보여줄 접속 상태 문자열을 결정합니다.
std::string visible_status_for(const Client* client) {
    if (!client->profile_public || !client->status_public) return "hidden";
    if (client->status == "online" && std::time(NULL) - client->last_active > 300) return "away";
    return client->status.empty() ? "online" : client->status;
}

// 현재 방 사용자와 공개 가능한 프로필/상태 정보를 출력 문자열로 만듭니다.
std::string who_list_text(const std::string& room_name) {
    pthread_mutex_lock(&state_mutex);
    std::ostringstream oss;
    oss << "[users " << room_name << "]\n";
    for (Client* client : clients) {
        if (client->room == room_name) {
            oss << "- " << client->nickname
                << " profile=" << visible_profile_for(client)
                << " status=" << visible_status_for(client) << "\n";
        }
    }
    pthread_mutex_unlock(&state_mutex);
    return oss.str();
}

// 사용자가 특정 메시지를 볼 권한이 있는지 확인합니다.
bool client_can_see_message_unlocked(Client* client, const MessageRecord& record) {
    if (record.room_type == ROOM_PRIVATE) return client->room == record.room || client->nickname == record.owner;
    return client->room == record.room;
}

// 현재 생성된 방과 오픈링크 목록을 출력 문자열로 만듭니다.
std::string room_list_text() {
    pthread_mutex_lock(&state_mutex);
    std::ostringstream oss;
    oss << "[rooms]";
    for (const auto& pair : rooms) oss << " " << pair.first << "(" << room_type_to_string(pair.second.type) << ")";
    if (!open_links.empty()) {
        oss << " [openlinks]";
        for (const auto& pair : open_links) oss << " " << pair.first << "->" << pair.second;
    }
    oss << "\n";
    pthread_mutex_unlock(&state_mutex);
    return oss.str();
}

// 같은 방에 있는 클라이언트들에게 메시지를 전송합니다.
void broadcast_room(const std::string& room_name, const std::string& message, int except_sock = -1) {
    pthread_mutex_lock(&state_mutex);
    for (Client* client : clients) {
        if (client->room == room_name && client->sock != except_sock) send_all(client->sock, message);
    }
    pthread_mutex_unlock(&state_mutex);
}

// 오픈링크 1:1 방 참여자와 링크 소유자에게 메시지를 전송합니다.
void broadcast_private_room(const std::string& room_name, const std::string& owner, const std::string& message, int except_sock = -1) {
    pthread_mutex_lock(&state_mutex);
    for (Client* client : clients) {
        if ((client->room == room_name || client->nickname == owner) && client->sock != except_sock) send_all(client->sock, message);
    }
    pthread_mutex_unlock(&state_mutex);
}

// 메시지 기록의 방 타입에 맞춰 반응, 수정, 삭제 알림을 전파합니다.
void broadcast_for_message(const MessageRecord& record, const std::string& text) {
    if (record.room_type == ROOM_PRIVATE) broadcast_private_room(record.room, record.owner, text);
    else broadcast_room(record.room, text);
}

// 접속 종료된 클라이언트를 서버의 클라이언트 목록에서 제거합니다.
void remove_client(Client* target) {
    pthread_mutex_lock(&state_mutex);
    clients.erase(std::remove(clients.begin(), clients.end(), target), clients.end());
    if (!target->username.empty() && accounts.count(target->username) > 0) {
        accounts[target->username].last_seen = std::time(NULL);
        save_state_unlocked();
    }
    pthread_mutex_unlock(&state_mutex);
}

// 계정 로그인 상태의 클라이언트 변경 사항을 계정 저장 정보에 반영합니다.
void sync_account_from_client_unlocked(Client* client) {
    if (client->username.empty() || accounts.count(client->username) == 0) return;
    Account& account = accounts[client->username];
    account.nickname = client->nickname;
    account.profile = client->profile;
    account.status = client->status;
    account.profile_public = client->profile_public;
    account.status_public = client->status_public;
    account.last_seen = std::time(NULL);
}

// 멘션된 접속자에게 방 알림을 남기고 뮤트 상태가 아니면 즉시 알려줍니다.
void notify_mentions(const MessageRecord& record) {
    pthread_mutex_lock(&state_mutex);
    for (Client* target : clients) {
        if (target->nickname == record.sender) continue;
        std::string token = "@" + target->nickname;
        if (record.text.find(token) == std::string::npos) continue;
        std::ostringstream notice;
        notice << "[mention] " << record.sender << " mentioned you in " << record.room << " #" << record.id << "\n";
        target->mentions.push_back(notice.str());
        if (target->mentions.size() > 30) target->mentions.erase(target->mentions.begin());
        if (target->muted_rooms.count(record.room) == 0) send_all(target->sock, notice.str());
    }
    pthread_mutex_unlock(&state_mutex);
}

// 메시지 기록을 히스토리 출력용 한 줄 문자열로 변환합니다.
std::string format_history_record(const MessageRecord& record) {
    std::ostringstream oss;
    oss << "#" << record.id << " " << record.time_text << " " << record.shown_sender << ": ";
    if (record.deleted) oss << "<deleted>";
    else oss << record.text;
    if (record.reply_to > 0) oss << " (reply #" << record.reply_to << ")";
    if (record.edited) oss << " (edited)";
    if (record.pinned) oss << " [pinned by " << record.pinned_by << "]";
    oss << "\n";
    return oss.str();
}

// 권한을 확인한 뒤 메시지를 기록하고 해당 방에 채팅으로 전송합니다.
bool send_chat_text(Client* client, const std::string& text, int reply_to = 0) {
    MessageRecord record;
    bool allowed;

    pthread_mutex_lock(&state_mutex);
    client->last_active = std::time(NULL);
    sync_account_from_client_unlocked(client);
    Room& room = rooms[client->room];
    allowed = can_speak_unlocked(room, client->nickname);
    if (!allowed) {
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("말할 권한이 없습니다."));
        return false;
    }
    record.id = next_message_id++;
    record.room = room.name;
    record.owner = room.owner;
    record.room_type = room.type;
    record.sender = client->nickname;
    record.shown_sender = display_name_unlocked(room, client->nickname);
    record.text = text;
    record.time_text = current_time_text();
    record.reply_to = reply_to;
    record.edited = false;
    record.deleted = false;
    record.pinned = false;
    messages[record.id] = record;
    message_order.push_back(record.id);
    pthread_mutex_unlock(&state_mutex);

    std::ostringstream oss;
    oss << "[" << record.room << "] " << record.shown_sender << ": " << text << " {" << record.time_text << "}\n";
    if (record.room_type == ROOM_PRIVATE) broadcast_private_room(record.room, record.owner, oss.str());
    else broadcast_room(record.room, oss.str());
    notify_mentions(record);
    return true;
}

// 클라이언트를 지정한 방으로 이동시키고 입퇴장 알림을 전송합니다.
void join_room(Client* client, const std::string& room_name) {
    std::string old_room;
    std::string nickname;
    pthread_mutex_lock(&state_mutex);
    old_room = client->room;
    nickname = client->nickname;
    client->room = room_name;
    client->last_active = std::time(NULL);
    pthread_mutex_unlock(&state_mutex);
    broadcast_room(old_room, format_notice_line(nickname + " left the room."), client->sock);
    broadcast_room(room_name, format_notice_line(nickname + " joined the room."), client->sock);
    send_to_client(client, format_notice_line("joined " + room_name));
}

// 관리자 권한이 필요한 명령에서 권한 여부를 검사합니다.
bool require_admin(Client* client, Room& room) {
    if (!is_admin_unlocked(room, client->nickname)) {
        send_to_client(client, format_notice_line("관리자 권한이 필요합니다."));
        return false;
    }
    return true;
}

// 방장 권한이 필요한 명령에서 권한 여부를 검사합니다.
bool require_owner(Client* client, Room& room) {
    if (!is_owner_unlocked(room, client->nickname)) {
        send_to_client(client, format_notice_line("방장 권한이 필요합니다."));
        return false;
    }
    return true;
}

// 일반/숨김/공지/승인/비밀 방 생성 명령을 처리합니다.
void create_room_command(Client* client, const std::string& name, RoomType type, const std::string& password = "") {
    std::string room_name = trim_copy(name);
    if (room_name.empty()) {
        send_to_client(client, format_notice_line("방 이름을 입력하세요."));
        return;
    }
    if (type == ROOM_SECRET && password.empty()) {
        send_to_client(client, format_notice_line("비밀방 비밀번호를 입력하세요."));
        return;
    }
    pthread_mutex_lock(&state_mutex);
    if (rooms.count(room_name) == 0) {
        rooms[room_name] = make_room(room_name, type, client->nickname, password);
        add_mod_log_unlocked(room_name, client->nickname + " created room");
        save_state_unlocked();
    }
    pthread_mutex_unlock(&state_mutex);
    join_room(client, room_name);
}

// 방 입장 요청과 비밀방 비밀번호, 차단 여부를 검증합니다.
void handle_join(Client* client, const std::string& room_name, const std::string& password = "") {
    std::string name = trim_copy(room_name);
    if (name.empty()) {
        send_to_client(client, format_notice_line("방 이름을 입력하세요."));
        return;
    }
    pthread_mutex_lock(&state_mutex);
    auto it = rooms.find(name);
    if (it == rooms.end()) {
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("존재하지 않는 방입니다."));
        return;
    }
    Room& room = it->second;
    if (room.banned.count(client->nickname) > 0) {
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("차단된 방입니다."));
        return;
    }
    if (room.type == ROOM_SECRET && room.password_hash != hash_password(password)) {
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("비밀번호가 틀렸습니다."));
        return;
    }
    if (room.type == ROOM_SECRET) {
        room.permitted.insert(client->nickname);
        save_state_unlocked();
    }
    pthread_mutex_unlock(&state_mutex);
    join_room(client, name);
}

// 오픈채팅 링크 이름과 소유자를 등록합니다.
void handle_openlink(Client* client, const std::string& link_name) {
    std::string link = trim_copy(link_name);
    if (link.empty()) {
        send_to_client(client, format_notice_line("오픈링크 이름을 입력하세요."));
        return;
    }
    pthread_mutex_lock(&state_mutex);
    open_links[link] = client->nickname;
    save_state_unlocked();
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, format_notice_line("openlink " + link + " created"));
}

// 오픈채팅 링크를 닫아 새 1:1 방 생성을 막습니다.
void handle_closelink(Client* client, const std::string& link_name) {
    std::string link = trim_copy(link_name);
    pthread_mutex_lock(&state_mutex);
    auto it = open_links.find(link);
    if (it == open_links.end() || it->second != client->nickname) {
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("닫을 수 없는 오픈링크입니다."));
        return;
    }
    open_links.erase(it);
    open_link_passwords.erase(link);
    open_link_banned.erase(link);
    save_state_unlocked();
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, format_notice_line("openlink " + link + " closed"));
}

// 오픈링크로 생성된 1:1 방 목록을 링크 소유자에게 보여줍니다.
void handle_linkrooms(Client* client, const std::string& link_name) {
    std::string link = trim_copy(link_name);
    pthread_mutex_lock(&state_mutex);
    std::ostringstream oss;
    oss << "[linkrooms " << link << "]";
    for (const auto& pair : private_room_links) {
        if (pair.second == link && rooms.count(pair.first) > 0 && rooms[pair.first].owner == client->nickname) oss << " " << pair.first;
    }
    oss << "\n";
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, oss.str());
}

// 오픈채팅 링크 입장자를 소유자와의 1:1 방으로 연결합니다.
void handle_enterlink(Client* client, const std::string& link_name) {
    std::string password;
    std::string link = first_token(link_name, &password);
    std::string owner;
    std::string room_name;
    pthread_mutex_lock(&state_mutex);
    auto it = open_links.find(link);
    if (it == open_links.end()) {
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("존재하지 않는 오픈링크입니다."));
        return;
    }
    owner = it->second;
    if (open_link_banned[link].count(client->nickname) > 0) {
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("blocked openlink."));
        return;
    }
    if (open_link_passwords.count(link) > 0 && open_link_passwords[link] != hash_password(password)) {
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("openlink password mismatch."));
        return;
    }
    room_name = link + "__" + client->nickname;
    if (rooms.count(room_name) == 0) {
        rooms[room_name] = make_room(room_name, ROOM_PRIVATE, owner);
        rooms[room_name].permitted.insert(client->nickname);
        private_room_links[room_name] = link;
        save_state_unlocked();
    }
    Client* owner_client = find_client_by_nick_unlocked(owner);
    if (owner_client != NULL) send_all(owner_client->sock, format_notice_line("openlink " + link + " new room " + room_name));
    pthread_mutex_unlock(&state_mutex);
    join_room(client, room_name);
}

// 오픈링크 입장 비밀번호를 설정합니다.
void handle_set_link_password(Client* client, const std::string& arg) {
    std::string password;
    std::string link = first_token(arg, &password);
    pthread_mutex_lock(&state_mutex);
    if (open_links.count(link) == 0 || open_links[link] != client->nickname || password.empty()) {
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("usage: /set_link_password <link> <password>"));
        return;
    }
    open_link_passwords[link] = hash_password(password);
    save_state_unlocked();
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, format_notice_line("openlink password set " + link));
}

// 오픈링크 입장 비밀번호를 제거합니다.
void handle_clear_link_password(Client* client, const std::string& link_arg) {
    std::string link = trim_copy(link_arg);
    pthread_mutex_lock(&state_mutex);
    if (open_links.count(link) == 0 || open_links[link] != client->nickname) {
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("cannot clear openlink password."));
        return;
    }
    open_link_passwords.erase(link);
    save_state_unlocked();
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, format_notice_line("openlink password cleared " + link));
}

// 오픈링크에 특정 닉네임의 입장을 차단하거나 해제합니다.
void handle_link_block_command(Client* client, const std::string& command, const std::string& arg) {
    std::string target;
    std::string link = first_token(arg, &target);
    pthread_mutex_lock(&state_mutex);
    if (open_links.count(link) == 0 || open_links[link] != client->nickname || target.empty()) {
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("usage: /link_block|/link_unblock <link> <nick>"));
        return;
    }
    if (command == "/link_block") open_link_banned[link].insert(target);
    else open_link_banned[link].erase(target);
    save_state_unlocked();
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, format_notice_line(link + " " + (command == "/link_block" ? "blocked " : "unblocked ") + target));
}

// 현재 1:1 오픈링크 방을 닫고 참여자를 lobby로 이동시킵니다.
void handle_close_private(Client* client) {
    std::vector<Client*> moved;
    std::string room_name;
    pthread_mutex_lock(&state_mutex);
    room_name = client->room;
    if (rooms.count(room_name) == 0 || rooms[room_name].type != ROOM_PRIVATE || rooms[room_name].owner != client->nickname) {
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("닫을 수 없는 1:1 방입니다."));
        return;
    }
    for (Client* peer : clients) {
        if (peer->room == room_name) {
            peer->room = "lobby";
            moved.push_back(peer);
        }
    }
    rooms.erase(room_name);
    private_room_links.erase(room_name);
    save_state_unlocked();
    pthread_mutex_unlock(&state_mutex);
    for (Client* peer : moved) send_to_client(peer, format_notice_line("1:1 방이 닫혀 lobby로 이동했습니다."));
}

// 허가, 회수, 관리자 위임, 강퇴, 차단 같은 방 관리 명령을 처리합니다.
void handle_permission_command(Client* client, const std::string& command, const std::string& target_nick) {
    std::string target = trim_copy(target_nick);
    if (target.empty()) {
        send_to_client(client, format_notice_line("대상 닉네임을 입력하세요."));
        return;
    }
    pthread_mutex_lock(&state_mutex);
    Room& room = rooms[client->room];
    if (!require_admin(client, room)) {
        pthread_mutex_unlock(&state_mutex);
        return;
    }
    if (command == "/permit" || command == "/approve") {
        room.permitted.insert(target);
        room.speak_requests.erase(target);
        add_mod_log_unlocked(room.name, client->nickname + " approved " + target);
        save_state_unlocked();
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line(target + " 발언 권한 승인"));
    } else if (command == "/deny") {
        room.speak_requests.erase(target);
        add_mod_log_unlocked(room.name, client->nickname + " denied " + target);
        save_state_unlocked();
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line(target + " 발언 요청 거절"));
    } else if (command == "/revoke") {
        room.permitted.erase(target);
        add_mod_log_unlocked(room.name, client->nickname + " revoked " + target);
        save_state_unlocked();
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line(target + " 발언 권한 회수"));
    } else if (command == "/delegate") {
        room.admins.insert(target);
        room.permitted.insert(target);
        add_mod_log_unlocked(room.name, client->nickname + " delegated " + target);
        save_state_unlocked();
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line(target + " 관리자 위임"));
    } else if (command == "/undelegate") {
        if (target == room.owner) {
            pthread_mutex_unlock(&state_mutex);
            send_to_client(client, format_notice_line("방장은 관리자 해임 대상이 아닙니다."));
            return;
        }
        room.admins.erase(target);
        add_mod_log_unlocked(room.name, client->nickname + " undelegated " + target);
        save_state_unlocked();
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line(target + " 관리자 해임"));
    } else if (command == "/kick" || command == "/ban") {
        std::string room_name = room.name;
        if (command == "/ban") room.banned.insert(target);
        room.speak_requests.erase(target);
        if (command == "/ban") add_mod_log_unlocked(room.name, client->nickname + " banned " + target);
        else add_mod_log_unlocked(room.name, client->nickname + " kicked " + target);
        Client* target_client = find_client_by_nick_unlocked(target);
        int target_sock = -1;
        if (target_client != NULL && target_client->room == room_name) {
            target_client->room = "lobby";
            target_sock = target_client->sock;
        }
        save_state_unlocked();
        pthread_mutex_unlock(&state_mutex);
        if (target_sock >= 0) {
            if (command == "/ban") send_all(target_sock, format_notice_line("차단되어 lobby로 이동했습니다."));
            else send_all(target_sock, format_notice_line("강퇴되어 lobby로 이동했습니다."));
        }
        broadcast_room(room_name, format_notice_line(target + "님이 " + client->nickname + "님에 의해 " + (command == "/ban" ? "차단" : "강퇴") + "되었습니다."));
        send_to_client(client, format_notice_line(target + (command == "/ban" ? " ban 처리" : " 강퇴 처리")));
    } else if (command == "/unban") {
        room.banned.erase(target);
        add_mod_log_unlocked(room.name, client->nickname + " unbanned " + target);
        save_state_unlocked();
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line(target + " unban 처리"));
    } else {
        pthread_mutex_unlock(&state_mutex);
    }
}

// 승인방에서 사용자가 자신의 발언 권한을 관리자에게 요청합니다.
void handle_request_speak(Client* client) {
    std::vector<int> admin_socks;
    std::string room_name;
    pthread_mutex_lock(&state_mutex);
    Room& room = rooms[client->room];
    room_name = room.name;
    if (room.type != ROOM_APPROVAL) {
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("승인방에서만 발언 요청을 사용할 수 있습니다."));
        return;
    }
    room.speak_requests.insert(client->nickname);
    for (Client* peer : clients) {
        if (peer->room == room.name && is_admin_unlocked(room, peer->nickname)) admin_socks.push_back(peer->sock);
    }
    save_state_unlocked();
    pthread_mutex_unlock(&state_mutex);
    for (int sock : admin_socks) send_all(sock, format_notice_line("speak request " + client->nickname + " in " + room_name));
    send_to_client(client, format_notice_line("발언 권한을 요청했습니다."));
}

// 현재 방의 발언 권한 요청 목록을 관리자에게 보여줍니다.
void handle_requests(Client* client) {
    pthread_mutex_lock(&state_mutex);
    Room& room = rooms[client->room];
    if (!require_admin(client, room)) {
        pthread_mutex_unlock(&state_mutex);
        return;
    }
    std::ostringstream oss;
    oss << "[requests " << room.name << "] " << join_set(room.speak_requests) << "\n";
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, oss.str());
}

// 현재 방의 차단 목록을 관리자에게 보여줍니다.
void handle_banlist(Client* client) {
    pthread_mutex_lock(&state_mutex);
    Room& room = rooms[client->room];
    if (!require_admin(client, room)) {
        pthread_mutex_unlock(&state_mutex);
        return;
    }
    std::string text = "[banlist " + room.name + "] " + join_set(room.banned) + "\n";
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, text);
}

// 현재 방의 방장 권한을 다른 사용자에게 이전합니다.
void handle_transfer_owner(Client* client, const std::string& target_arg) {
    std::string target = trim_copy(target_arg);
    pthread_mutex_lock(&state_mutex);
    Room& room = rooms[client->room];
    if (!require_owner(client, room) || target.empty()) {
        pthread_mutex_unlock(&state_mutex);
        if (target.empty()) send_to_client(client, format_notice_line("usage: /transfer_owner <nick>"));
        return;
    }
    room.owner = target;
    room.admins.insert(target);
    room.permitted.insert(target);
    add_mod_log_unlocked(room.name, client->nickname + " transferred owner to " + target);
    save_state_unlocked();
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, format_notice_line("owner transferred to " + target));
}

// 현재 방의 타입, 소유자, 관리자, 허가 목록을 출력합니다.
void handle_roominfo(Client* client) {
    pthread_mutex_lock(&state_mutex);
    Room room = rooms[client->room];
    std::ostringstream oss;
    oss << "[roominfo] " << room.name
        << " type=" << room_type_to_string(room.type)
        << " owner=" << room.owner
        << " admins=" << join_set(room.admins)
        << " permitted=" << join_set(room.permitted)
        << " banned=" << join_set(room.banned)
        << " requests=" << join_set(room.speak_requests)
        << "\n";
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, oss.str());
}

// 현재 방의 운영 로그를 관리자에게 보여줍니다.
void handle_modlog(Client* client) {
    pthread_mutex_lock(&state_mutex);
    Room& room = rooms[client->room];
    if (!require_admin(client, room)) {
        pthread_mutex_unlock(&state_mutex);
        return;
    }
    std::vector<std::string> log = mod_logs[room.name];
    pthread_mutex_unlock(&state_mutex);
    std::ostringstream oss;
    oss << "[modlog]\n";
    for (const std::string& entry : log) oss << entry << "\n";
    send_to_client(client, oss.str());
}

// 현재 방의 이름을 변경하고 접속자와 메시지 기록의 방 이름을 갱신합니다.
void handle_rename_room(Client* client, const std::string& new_name_arg) {
    std::string new_name = trim_copy(new_name_arg);
    if (new_name.empty()) {
        send_to_client(client, format_notice_line("사용법: /rename_room <새이름>"));
        return;
    }
    pthread_mutex_lock(&state_mutex);
    std::string old_name = client->room;
    Room& old_room = rooms[old_name];
    if (!require_owner(client, old_room)) {
        pthread_mutex_unlock(&state_mutex);
        return;
    }
    if (old_name == "lobby" || rooms.count(new_name) > 0) {
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("변경할 수 없는 방 이름입니다."));
        return;
    }
    Room room = old_room;
    room.name = new_name;
    rooms.erase(old_name);
    rooms[new_name] = room;
    for (Client* peer : clients) if (peer->room == old_name) peer->room = new_name;
    for (auto& pair : messages) if (pair.second.room == old_name) pair.second.room = new_name;
    if (private_room_links.count(old_name) > 0) {
        private_room_links[new_name] = private_room_links[old_name];
        private_room_links.erase(old_name);
    }
    mod_logs[new_name] = mod_logs[old_name];
    mod_logs.erase(old_name);
    add_mod_log_unlocked(new_name, client->nickname + " renamed room from " + old_name);
    save_state_unlocked();
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, format_notice_line("renamed " + old_name + " -> " + new_name));
}

// 현재 방을 삭제하고 방 안의 접속자를 lobby로 이동시킵니다.
void handle_delete_room(Client* client) {
    std::vector<Client*> moved;
    std::string room_name;
    pthread_mutex_lock(&state_mutex);
    room_name = client->room;
    Room& room = rooms[room_name];
    if (!require_owner(client, room)) {
        pthread_mutex_unlock(&state_mutex);
        return;
    }
    if (room_name == "lobby") {
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("lobby는 삭제할 수 없습니다."));
        return;
    }
    for (Client* peer : clients) {
        if (peer->room == room_name) {
            peer->room = "lobby";
            moved.push_back(peer);
        }
    }
    rooms.erase(room_name);
    private_room_links.erase(room_name);
    mod_logs.erase(room_name);
    for (Client* peer : clients) peer->muted_rooms.erase(room_name);
    std::vector<int> kept_order;
    for (int id : message_order) {
        if (messages.count(id) > 0 && messages[id].room == room_name) {
            messages.erase(id);
            reactions.erase(id);
        } else {
            kept_order.push_back(id);
        }
    }
    message_order = kept_order;
    save_state_unlocked();
    pthread_mutex_unlock(&state_mutex);
    for (Client* peer : moved) send_to_client(peer, format_notice_line("deleted room " + room_name + "; moved to lobby"));
}

// 현재 방의 타입을 방장이 변경합니다.
void handle_set_room_type(Client* client, const std::string& type_arg) {
    std::string type_text = trim_copy(type_arg);
    RoomType type = parse_room_type(type_text);
    if (type_text != "normal" && type_text != "hidden" && type_text != "announce" && type_text != "approval" && type_text != "secret") {
        send_to_client(client, format_notice_line("사용법: /set_room_type normal|hidden|announce|approval|secret"));
        return;
    }
    pthread_mutex_lock(&state_mutex);
    Room& room = rooms[client->room];
    if (!require_owner(client, room)) {
        pthread_mutex_unlock(&state_mutex);
        return;
    }
    if (type == ROOM_SECRET && room.password_hash.empty()) {
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("비밀방 전환 전 /set_room_password <비밀번호>를 사용하세요."));
        return;
    }
    room.type = type;
    add_mod_log_unlocked(room.name, client->nickname + " set type " + room_type_to_string(type));
    save_state_unlocked();
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, format_notice_line("room type set to " + room_type_to_string(type)));
}

// 현재 방의 비밀방 비밀번호를 설정하고 타입을 비밀방으로 바꿉니다.
void handle_set_room_password(Client* client, const std::string& password_arg) {
    std::string password = trim_copy(password_arg);
    if (password.empty()) {
        send_to_client(client, format_notice_line("사용법: /set_room_password <비밀번호>"));
        return;
    }
    pthread_mutex_lock(&state_mutex);
    Room& room = rooms[client->room];
    if (!require_owner(client, room)) {
        pthread_mutex_unlock(&state_mutex);
        return;
    }
    room.password_hash = hash_password(password);
    room.type = ROOM_SECRET;
    add_mod_log_unlocked(room.name, client->nickname + " set room password");
    save_state_unlocked();
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, format_notice_line("비밀방 비밀번호가 설정되었습니다."));
}

// 현재 방의 비밀번호를 제거하고 일반방으로 바꿉니다.
void handle_clear_room_password(Client* client) {
    pthread_mutex_lock(&state_mutex);
    Room& room = rooms[client->room];
    if (!require_owner(client, room)) {
        pthread_mutex_unlock(&state_mutex);
        return;
    }
    room.password_hash.clear();
    if (room.type == ROOM_SECRET) room.type = ROOM_NORMAL;
    add_mod_log_unlocked(room.name, client->nickname + " cleared room password");
    save_state_unlocked();
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, format_notice_line("비밀방 비밀번호가 제거되었습니다."));
}

// 개인별 자주 쓰는 문장 템플릿 추가, 삭제, 조회, 사용을 처리합니다.
void handle_template_command(Client* client, const std::string& arg) {
    std::string rest;
    std::string subcommand = first_token(arg, &rest);
    if (subcommand == "add") {
        std::string text;
        std::string key = first_token(rest, &text);
        if (key.empty() || text.empty()) {
            send_to_client(client, format_notice_line("사용법: /template add <키워드> <문장>"));
            return;
        }
        pthread_mutex_lock(&state_mutex);
        templates[client->nickname][key] = text;
        save_state_unlocked();
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("template " + key + " saved"));
    } else if (subcommand == "del") {
        std::string key = trim_copy(rest);
        pthread_mutex_lock(&state_mutex);
        templates[client->nickname].erase(key);
        save_state_unlocked();
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("template " + key + " deleted"));
    } else if (subcommand == "list") {
        pthread_mutex_lock(&state_mutex);
        std::map<std::string, std::string> user_templates = templates[client->nickname];
        pthread_mutex_unlock(&state_mutex);
        std::ostringstream oss;
        oss << "[templates]\n";
        for (const auto& pair : user_templates) oss << pair.first << " = " << pair.second << "\n";
        send_to_client(client, oss.str());
    } else if (subcommand == "suggest") {
        std::string prefix = trim_copy(rest);
        pthread_mutex_lock(&state_mutex);
        std::map<std::string, std::string> user_templates = templates[client->nickname];
        pthread_mutex_unlock(&state_mutex);
        std::ostringstream oss;
        oss << "[template suggestions]\n";
        for (const auto& pair : user_templates) if (pair.first.rfind(prefix, 0) == 0) oss << pair.first << " = " << pair.second << "\n";
        send_to_client(client, oss.str());
    } else if (subcommand == "use") {
        std::string key = trim_copy(rest);
        std::string text;
        pthread_mutex_lock(&state_mutex);
        auto owner_it = templates.find(client->nickname);
        if (owner_it != templates.end()) {
            auto template_it = owner_it->second.find(key);
            if (template_it != owner_it->second.end()) text = template_it->second;
        }
        pthread_mutex_unlock(&state_mutex);
        if (text.empty()) {
            send_to_client(client, format_notice_line("템플릿을 찾을 수 없습니다: " + key));
            return;
        }
        send_chat_text(client, text);
    } else {
        send_to_client(client, format_notice_line("사용법: /template add|del|list|suggest|use ..."));
    }
}

// 현재 방에서 볼 수 있는 최근 메시지를 ID와 함께 보여줍니다.
void handle_history(Client* client, const std::string& arg) {
    int limit = parse_history_limit(arg);
    std::vector<MessageRecord> selected;
    std::string room_name;
    pthread_mutex_lock(&state_mutex);
    room_name = client->room;
    for (auto it = message_order.rbegin(); it != message_order.rend() && static_cast<int>(selected.size()) < limit; ++it) {
        MessageRecord record = messages[*it];
        if (client_can_see_message_unlocked(client, record)) selected.push_back(record);
    }
    pthread_mutex_unlock(&state_mutex);
    std::reverse(selected.begin(), selected.end());
    std::ostringstream oss;
    oss << "[history " << room_name << "]\n";
    for (const MessageRecord& record : selected) oss << format_history_record(record);
    send_to_client(client, oss.str());
}

// 현재 방에서 키워드가 포함된 메시지를 검색합니다.
void handle_search(Client* client, const std::string& keyword_arg) {
    std::string keyword = trim_copy(keyword_arg);
    if (keyword.empty()) {
        send_to_client(client, format_notice_line("usage: /search <keyword>"));
        return;
    }
    std::vector<MessageRecord> selected;
    std::string room_name;
    pthread_mutex_lock(&state_mutex);
    room_name = client->room;
    for (int id : message_order) {
        MessageRecord record = messages[id];
        if (!record.deleted && client_can_see_message_unlocked(client, record) && record.text.find(keyword) != std::string::npos) selected.push_back(record);
    }
    pthread_mutex_unlock(&state_mutex);
    std::ostringstream oss;
    oss << "[search " << room_name << " " << keyword << "]\n";
    for (const MessageRecord& record : selected) oss << format_history_record(record);
    send_to_client(client, oss.str());
}

// 현재 방에서 지정 메시지 ID 이후의 메시지를 보여줍니다.
void handle_since(Client* client, const std::string& id_arg) {
    int after_id = atoi(trim_copy(id_arg).c_str());
    std::vector<MessageRecord> selected;
    std::string room_name;
    pthread_mutex_lock(&state_mutex);
    room_name = client->room;
    for (int id : message_order) {
        MessageRecord record = messages[id];
        if (id > after_id && client_can_see_message_unlocked(client, record)) selected.push_back(record);
    }
    pthread_mutex_unlock(&state_mutex);
    std::ostringstream oss;
    oss << "[since " << room_name << " #" << after_id << "]\n";
    for (const MessageRecord& record : selected) oss << format_history_record(record);
    send_to_client(client, oss.str());
}

// 메시지에 이모지 반응을 등록하고 볼 수 있는 사용자에게 알립니다.
void handle_react(Client* client, const std::string& arg) {
    std::string emoji;
    std::string id_text = first_token(arg, &emoji);
    if (id_text.empty() || emoji.empty()) {
        send_to_client(client, format_notice_line("사용법: /react <메시지ID> <이모지> (/history로 ID 확인)"));
        return;
    }
    int id = atoi(id_text.c_str());
    MessageRecord record;
    bool found = false;
    pthread_mutex_lock(&state_mutex);
    auto it = messages.find(id);
    if (it != messages.end() && !it->second.deleted && client_can_see_message_unlocked(client, it->second)) {
        reactions[id][client->nickname] = emoji;
        record = it->second;
        found = true;
    }
    pthread_mutex_unlock(&state_mutex);
    if (!found) {
        send_to_client(client, format_notice_line("반응할 수 없는 메시지입니다."));
        return;
    }
    std::ostringstream oss;
    oss << "[reaction] #" << id << " " << emoji << " by " << client->nickname << "\n";
    broadcast_for_message(record, oss.str());
}

// 특정 메시지에 등록된 이모지 반응 목록을 조회합니다.
void handle_reactions(Client* client, const std::string& arg) {
    int id = atoi(trim_copy(arg).c_str());
    std::map<std::string, std::string> snapshot;
    bool found = false;
    pthread_mutex_lock(&state_mutex);
    auto it = messages.find(id);
    if (it != messages.end() && client_can_see_message_unlocked(client, it->second)) {
        snapshot = reactions[id];
        found = true;
    }
    pthread_mutex_unlock(&state_mutex);
    if (!found) {
        send_to_client(client, format_notice_line("조회할 수 없는 메시지입니다."));
        return;
    }
    std::ostringstream oss;
    oss << "[reactions #" << id << "]";
    for (const auto& pair : snapshot) oss << " " << pair.first << "=" << pair.second;
    oss << "\n";
    send_to_client(client, oss.str());
}

// 특정 메시지에 답장 형식으로 새 메시지를 작성합니다.
void handle_reply(Client* client, const std::string& arg) {
    std::string reply_text;
    std::string id_text = first_token(arg, &reply_text);
    int id = atoi(id_text.c_str());
    bool found = false;
    pthread_mutex_lock(&state_mutex);
    auto it = messages.find(id);
    if (it != messages.end() && !it->second.deleted && client_can_see_message_unlocked(client, it->second)) found = true;
    pthread_mutex_unlock(&state_mutex);
    if (!found || reply_text.empty()) {
        send_to_client(client, format_notice_line("사용법: /reply <메시지ID> <내용> (/history로 ID 확인)"));
        return;
    }
    send_chat_text(client, "[reply #" + std::to_string(id) + "] " + reply_text, id);
}

// 본인 또는 관리자가 기존 메시지 내용을 수정합니다.
void handle_edit(Client* client, const std::string& arg) {
    std::string new_text;
    std::string id_text = first_token(arg, &new_text);
    int id = atoi(id_text.c_str());
    MessageRecord record;
    bool allowed = false;
    pthread_mutex_lock(&state_mutex);
    auto it = messages.find(id);
    if (it != messages.end() && !it->second.deleted && client_can_see_message_unlocked(client, it->second)) {
        Room& room = rooms[it->second.room];
        if (it->second.sender == client->nickname || is_admin_unlocked(room, client->nickname)) {
            it->second.text = new_text;
            it->second.edited = true;
            record = it->second;
            allowed = true;
        }
    }
    pthread_mutex_unlock(&state_mutex);
    if (!allowed || new_text.empty()) {
        send_to_client(client, format_notice_line("수정할 수 없는 메시지입니다. /history로 ID를 확인하세요."));
        return;
    }
    broadcast_for_message(record, format_notice_line("edited #" + std::to_string(id) + " by " + client->nickname));
}

// 본인 또는 관리자가 기존 메시지를 삭제 표시합니다.
void handle_delete_message(Client* client, const std::string& arg) {
    int id = atoi(trim_copy(arg).c_str());
    MessageRecord record;
    bool allowed = false;
    pthread_mutex_lock(&state_mutex);
    auto it = messages.find(id);
    if (it != messages.end() && client_can_see_message_unlocked(client, it->second)) {
        Room& room = rooms[it->second.room];
        if (it->second.sender == client->nickname || is_admin_unlocked(room, client->nickname)) {
            it->second.deleted = true;
            it->second.text = "";
            record = it->second;
            allowed = true;
        }
    }
    pthread_mutex_unlock(&state_mutex);
    if (!allowed) {
        send_to_client(client, format_notice_line("삭제할 수 없는 메시지입니다. /history로 ID를 확인하세요."));
        return;
    }
    broadcast_for_message(record, format_notice_line("deleted #" + std::to_string(id) + " by " + client->nickname));
}

// 메시지를 현재 방의 고정 메시지로 표시합니다.
void handle_pin(Client* client, const std::string& arg) {
    int id = atoi(trim_copy(arg).c_str());
    MessageRecord record;
    bool allowed = false;
    pthread_mutex_lock(&state_mutex);
    auto it = messages.find(id);
    if (it != messages.end() && client_can_see_message_unlocked(client, it->second)) {
        Room& room = rooms[it->second.room];
        if (room.type != ROOM_ANNOUNCE || is_admin_unlocked(room, client->nickname)) {
            it->second.pinned = true;
            it->second.pinned_by = client->nickname;
            record = it->second;
            allowed = true;
        }
    }
    pthread_mutex_unlock(&state_mutex);
    if (!allowed) {
        send_to_client(client, format_notice_line("pin할 수 없는 메시지입니다. /history로 ID를 확인하세요."));
        return;
    }
    broadcast_for_message(record, format_notice_line("pinned #" + std::to_string(id) + " by " + client->nickname));
}

// pin한 사람이나 방장이 메시지 고정을 해제합니다.
void handle_unpin(Client* client, const std::string& arg) {
    int id = atoi(trim_copy(arg).c_str());
    MessageRecord record;
    bool allowed = false;
    bool found = false;
    pthread_mutex_lock(&state_mutex);
    auto it = messages.find(id);
    if (it != messages.end() && client_can_see_message_unlocked(client, it->second)) {
        found = true;
        Room& room = rooms[it->second.room];
        if (it->second.pinned_by == client->nickname || room.owner == client->nickname) {
            it->second.pinned = false;
            it->second.pinned_by.clear();
            record = it->second;
            allowed = true;
        }
    }
    pthread_mutex_unlock(&state_mutex);
    if (!found || !allowed) {
        send_to_client(client, format_notice_line("pin한 사람 또는 방장만 unpin할 수 있습니다."));
        return;
    }
    broadcast_for_message(record, format_notice_line("unpinned #" + std::to_string(id) + " by " + client->nickname));
}

// 현재 방의 고정 메시지 목록을 보여줍니다.
void handle_pinned(Client* client) {
    std::vector<MessageRecord> selected;
    std::string room_name;
    pthread_mutex_lock(&state_mutex);
    room_name = client->room;
    for (int id : message_order) {
        MessageRecord record = messages[id];
        if (record.pinned && client_can_see_message_unlocked(client, record)) selected.push_back(record);
    }
    pthread_mutex_unlock(&state_mutex);
    std::ostringstream oss;
    oss << "[pinned " << room_name << "]\n";
    for (const MessageRecord& record : selected) oss << format_history_record(record);
    send_to_client(client, oss.str());
}

// 새 계정을 만들고 현재 접속자를 해당 계정으로 로그인 처리합니다.
void handle_register(Client* client, const std::string& arg) {
    std::string password;
    std::string username = first_token(arg, &password);
    if (username.empty() || password.empty()) {
        send_to_client(client, format_notice_line("사용법: /register <id> <password>"));
        return;
    }
    pthread_mutex_lock(&state_mutex);
    if (accounts.count(username) > 0) {
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("이미 존재하는 계정입니다."));
        return;
    }
    Account account;
    account.username = username;
    account.password_hash = hash_password(password);
    account.nickname = client->nickname;
    account.profile = client->profile;
    account.status = client->status;
    account.profile_public = client->profile_public;
    account.status_public = client->status_public;
    account.last_seen = std::time(NULL);
    accounts[username] = account;
    client->username = username;
    save_state_unlocked();
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, format_notice_line("registered " + username));
}

// 계정 비밀번호를 확인하고 프로필, 상태, 닉네임을 복원합니다.
void handle_login(Client* client, const std::string& arg) {
    std::string password;
    std::string username = first_token(arg, &password);
    if (username.empty() || password.empty()) {
        send_to_client(client, format_notice_line("사용법: /login <id> <password>"));
        return;
    }
    pthread_mutex_lock(&state_mutex);
    auto it = accounts.find(username);
    if (it == accounts.end() || it->second.password_hash != hash_password(password)) {
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("로그인 실패"));
        return;
    }
    Account& account = it->second;
    client->username = username;
    client->nickname = account.nickname;
    client->profile = account.profile;
    client->status = account.status.empty() ? "online" : account.status;
    client->profile_public = account.profile_public;
    client->status_public = account.status_public;
    client->last_active = std::time(NULL);
    account.last_seen = client->last_active;
    save_state_unlocked();
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, format_notice_line("logged in " + username));
}

// 닉네임 기준으로 온라인 또는 계정의 마지막 활동 시각을 조회합니다.
void handle_lastseen(Client* client, const std::string& arg) {
    std::string target = trim_copy(arg);
    pthread_mutex_lock(&state_mutex);
    Client* online = find_client_by_nick_unlocked(target);
    std::ostringstream oss;
    if (online != NULL) {
        oss << "[lastseen " << target << "] online, active " << (std::time(NULL) - online->last_active) << "s ago\n";
    } else {
        bool found = false;
        for (const auto& pair : accounts) {
            if (pair.second.nickname == target) {
                oss << "[lastseen " << target << "] offline, last " << (std::time(NULL) - pair.second.last_seen) << "s ago\n";
                found = true;
                break;
            }
        }
        if (!found) oss << "[lastseen " << target << "] not found\n";
    }
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, oss.str());
}

// 현재 방 또는 지정한 방의 멘션 알림을 뮤트합니다.
void handle_mute(Client* client, const std::string& arg) {
    pthread_mutex_lock(&state_mutex);
    std::string room = trim_copy(arg).empty() ? client->room : trim_copy(arg);
    client->muted_rooms.insert(room);
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, format_notice_line("muted " + room));
}

// 현재 방 또는 지정한 방의 멘션 알림 뮤트를 해제합니다.
void handle_unmute(Client* client, const std::string& arg) {
    pthread_mutex_lock(&state_mutex);
    std::string room = trim_copy(arg).empty() ? client->room : trim_copy(arg);
    client->muted_rooms.erase(room);
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, format_notice_line("unmuted " + room));
}

// 누적된 멘션 알림 목록을 보여주고 읽음 처리합니다.
void handle_mentions(Client* client) {
    pthread_mutex_lock(&state_mutex);
    std::vector<std::string> mentions = client->mentions;
    client->mentions.clear();
    pthread_mutex_unlock(&state_mutex);
    std::ostringstream oss;
    oss << "[mentions]\n";
    for (const std::string& mention : mentions) oss << mention;
    send_to_client(client, oss.str());
}

// 읽지 않은 개인 알림 수를 요약해서 보여줍니다.
void handle_unread(Client* client) {
    pthread_mutex_lock(&state_mutex);
    size_t mention_count = client->mentions.size();
    std::string muted = join_set(client->muted_rooms);
    pthread_mutex_unlock(&state_mutex);
    std::ostringstream oss;
    oss << "[unread] mentions=" << mention_count << " muted=" << muted << "\n";
    send_to_client(client, oss.str());
}

// 서버가 지원하는 명령어 목록을 클라이언트에게 전송합니다.
void send_help(Client* client) {
    send_to_client(client,
        "[commands]\n"
        "계정: /register <id> <pw> /login <id> <pw> /lastseen <nick>\n"
        "프로필: /nick <name> /profile <text> /status <online|away|busy> /profile_public <on|off> /status_public <on|off> /profileof <nick>\n"
        "방: /create <room> /create_hidden <room> /create_announce <room> /create_approval <room> /create_secret <room> <pw>\n"
        "입장: /join <room> /join_secret <room> <pw> /leave /rooms /who /roominfo\n"
        "관리: /request_speak /requests /approve <nick> /deny <nick> /permit <nick> /revoke <nick> /delegate <nick> /undelegate <nick>\n"
        "관리: /kick <nick> /ban <nick> /unban <nick> /banlist /modlog /rename_room <name> /delete_room /transfer_owner <nick>\n"
        "관리: /set_room_type normal|hidden|announce|approval|secret /set_room_password <pw> /clear_room_password\n"
        "오픈링크: /openlink <name> /enterlink <name> [pw] /closelink <name> /linkrooms <name> /close_private\n"
        "오픈링크: /set_link_password <link> <pw> /clear_link_password <link> /link_block <link> <nick> /link_unblock <link> <nick>\n"
        "메시지: /history [count] /recent [count] /search <keyword> /since <id> /reply <id> <msg> /edit <id> <msg> /delete <id> /pin <id> /unpin <id> /pinned\n"
        "반응/알림: /react <id> <emoji> /reactions <id> /mute [room] /unmute [room] /mentions /unread\n"
        "템플릿: /template add <key> <message> /template del <key> /template list /template suggest <prefix> /template use <key>\n"
        "종료: /quit\n");
}

// 슬래시로 시작하는 클라이언트 명령을 파싱해 해당 처리 함수로 전달합니다.
void handle_command(Client* client, const std::string& line) {
    std::string command;
    std::string arg;
    size_t space_pos = line.find(' ');
    if (space_pos == std::string::npos) command = line;
    else {
        command = line.substr(0, space_pos);
        arg = trim_copy(line.substr(space_pos + 1));
    }

    pthread_mutex_lock(&state_mutex);
    client->last_active = std::time(NULL);
    pthread_mutex_unlock(&state_mutex);

    if (command == "/help") {
        send_help(client);
    } else if (command == "/register") {
        handle_register(client, arg);
    } else if (command == "/login") {
        handle_login(client, arg);
    } else if (command == "/lastseen") {
        handle_lastseen(client, arg);
    } else if (command == "/nick") {
        if (arg.empty()) {
            send_to_client(client, format_notice_line("닉네임을 입력하세요."));
            return;
        }
        pthread_mutex_lock(&state_mutex);
        std::string old = client->nickname;
        client->nickname = arg;
        sync_account_from_client_unlocked(client);
        save_state_unlocked();
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line(old + " -> " + arg));
    } else if (command == "/profile") {
        pthread_mutex_lock(&state_mutex);
        client->profile = arg;
        sync_account_from_client_unlocked(client);
        save_state_unlocked();
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("프로필이 설정되었습니다: " + arg));
    } else if (command == "/status") {
        std::string value = trim_copy(arg);
        if (value != "online" && value != "away" && value != "busy") {
            send_to_client(client, format_notice_line("사용법: /status <online|away|busy>"));
            return;
        }
        pthread_mutex_lock(&state_mutex);
        client->status = value;
        sync_account_from_client_unlocked(client);
        save_state_unlocked();
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("접속 상태가 설정되었습니다: " + value));
    } else if (command == "/profile_public") {
        bool value;
        if (!parse_on_off(arg, &value)) {
            send_to_client(client, format_notice_line("사용법: /profile_public <on|off>"));
            return;
        }
        pthread_mutex_lock(&state_mutex);
        client->profile_public = value;
        sync_account_from_client_unlocked(client);
        save_state_unlocked();
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line(std::string("프로필 공개: ") + (value ? "on" : "off")));
    } else if (command == "/status_public") {
        bool value;
        if (!parse_on_off(arg, &value)) {
            send_to_client(client, format_notice_line("사용법: /status_public <on|off>"));
            return;
        }
        pthread_mutex_lock(&state_mutex);
        client->status_public = value;
        sync_account_from_client_unlocked(client);
        save_state_unlocked();
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line(std::string("접속 상태 공개: ") + (value ? "on" : "off")));
    } else if (command == "/profileof") {
        std::string target = trim_copy(arg);
        pthread_mutex_lock(&state_mutex);
        Client* target_client = find_client_by_nick_unlocked(target);
        std::ostringstream oss;
        if (target_client == NULL) oss << "[profile] " << target << " not found\n";
        else oss << "[profile] " << target_client->nickname << " profile=" << visible_profile_for(target_client) << " status=" << visible_status_for(target_client) << "\n";
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, oss.str());
    } else if (command == "/create") {
        create_room_command(client, arg, ROOM_NORMAL);
    } else if (command == "/create_hidden") {
        create_room_command(client, arg, ROOM_HIDDEN);
    } else if (command == "/create_announce") {
        create_room_command(client, arg, ROOM_ANNOUNCE);
    } else if (command == "/create_approval") {
        create_room_command(client, arg, ROOM_APPROVAL);
    } else if (command == "/create_secret" || command == "/create_secure") {
        std::string password;
        std::string room = first_token(arg, &password);
        if (room.empty() || password.empty()) send_to_client(client, format_notice_line("사용법: /create_secret <방이름> <비밀번호>"));
        else create_room_command(client, room, ROOM_SECRET, password);
    } else if (command == "/join") {
        handle_join(client, arg);
    } else if (command == "/join_secret" || command == "/join_secure") {
        std::string password;
        std::string room = first_token(arg, &password);
        if (room.empty() || password.empty()) send_to_client(client, format_notice_line("사용법: /join_secret <방이름> <비밀번호>"));
        else handle_join(client, room, password);
    } else if (command == "/openlink") {
        handle_openlink(client, arg);
    } else if (command == "/enterlink") {
        handle_enterlink(client, arg);
    } else if (command == "/closelink") {
        handle_closelink(client, arg);
    } else if (command == "/linkrooms") {
        handle_linkrooms(client, arg);
    } else if (command == "/set_link_password") {
        handle_set_link_password(client, arg);
    } else if (command == "/clear_link_password") {
        handle_clear_link_password(client, arg);
    } else if (command == "/link_block" || command == "/link_unblock") {
        handle_link_block_command(client, command, arg);
    } else if (command == "/close_private") {
        handle_close_private(client);
    } else if (command == "/permit" || command == "/revoke" || command == "/delegate" || command == "/undelegate" || command == "/kick" || command == "/ban" || command == "/unban" || command == "/approve" || command == "/deny") {
        handle_permission_command(client, command, arg);
    } else if (command == "/request_speak") {
        handle_request_speak(client);
    } else if (command == "/requests") {
        handle_requests(client);
    } else if (command == "/banlist") {
        handle_banlist(client);
    } else if (command == "/modlog") {
        handle_modlog(client);
    } else if (command == "/roominfo") {
        handle_roominfo(client);
    } else if (command == "/transfer_owner") {
        handle_transfer_owner(client, arg);
    } else if (command == "/rename_room") {
        handle_rename_room(client, arg);
    } else if (command == "/delete_room") {
        handle_delete_room(client);
    } else if (command == "/set_room_type") {
        handle_set_room_type(client, arg);
    } else if (command == "/set_room_password") {
        handle_set_room_password(client, arg);
    } else if (command == "/clear_room_password") {
        handle_clear_room_password(client);
    } else if (command == "/template") {
        handle_template_command(client, arg);
    } else if (command == "/history" || command == "/recent") {
        handle_history(client, arg);
    } else if (command == "/search") {
        handle_search(client, arg);
    } else if (command == "/since") {
        handle_since(client, arg);
    } else if (command == "/reply") {
        handle_reply(client, arg);
    } else if (command == "/edit") {
        handle_edit(client, arg);
    } else if (command == "/delete") {
        handle_delete_message(client, arg);
    } else if (command == "/pin") {
        handle_pin(client, arg);
    } else if (command == "/unpin") {
        handle_unpin(client, arg);
    } else if (command == "/pinned") {
        handle_pinned(client);
    } else if (command == "/react") {
        handle_react(client, arg);
    } else if (command == "/reactions") {
        handle_reactions(client, arg);
    } else if (command == "/mute") {
        handle_mute(client, arg);
    } else if (command == "/unmute") {
        handle_unmute(client, arg);
    } else if (command == "/mentions") {
        handle_mentions(client);
    } else if (command == "/unread") {
        handle_unread(client);
    } else if (command == "/leave") {
        pthread_mutex_lock(&state_mutex);
        std::string current_room = client->room;
        pthread_mutex_unlock(&state_mutex);
        if (current_room == "lobby") send_to_client(client, format_notice_line("이미 lobby에 있습니다."));
        else join_room(client, "lobby");
    } else if (command == "/rooms") {
        send_to_client(client, room_list_text());
    } else if (command == "/who") {
        pthread_mutex_lock(&state_mutex);
        std::string room = client->room;
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, who_list_text(room));
    } else if (command == "/quit") {
        shutdown(client->sock, SHUT_RDWR);
    } else {
        send_to_client(client, format_notice_line("알 수 없는 명령어입니다. /help 를 입력하세요."));
    }
}

// 클라이언트 입력 한 줄을 명령 또는 채팅 메시지로 분기합니다.
void handle_line(Client* client, const std::string& raw_line) {
    std::string line = trim_copy(raw_line);
    if (line.empty()) return;
    if (line[0] == '/') {
        handle_command(client, line);
        return;
    }
    send_chat_text(client, line);
}

// 클라이언트별 스레드에서 수신 루프와 접속 종료 처리를 담당합니다.
void* handle_clnt(void* arg) {
    Client* client = static_cast<Client*>(arg);
    char buffer[BUFSIZE];
    std::string pending;
    send_to_client(client, format_notice_line("connected. default room is lobby. /help"));
    broadcast_room("lobby", format_notice_line(client->nickname + " joined."), client->sock);

    while (true) {
        ssize_t read_len = read(client->sock, buffer, sizeof(buffer));
        if (read_len <= 0) break;
        pending.append(buffer, read_len);
        size_t newline_pos;
        while ((newline_pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline_pos);
            pending.erase(0, newline_pos + 1);
            handle_line(client, line);
        }
    }

    pthread_mutex_lock(&state_mutex);
    std::string nickname = client->nickname;
    std::string room = client->room;
    pthread_mutex_unlock(&state_mutex);
    remove_client(client);
    broadcast_room(room, format_notice_line(nickname + " disconnected."));
    close(client->sock);
    delete client;
    return NULL;
}

// 서버 소켓을 열고 클라이언트 접속마다 처리 스레드를 생성하는 서버 진입점입니다.
int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        printf("Usage : %s <port> [state_file]\n", argv[0]);
        exit(1);
    }
    if (argc == 3) state_path = argv[2];
    load_state();

    int serv_sock = socket(PF_INET, SOCK_STREAM, 0);
    if (serv_sock == -1) error_handling("socket() error");

    int option = 1;
    setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(atoi(argv[1]));

    if (bind(serv_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1) error_handling("bind() error");
    if (listen(serv_sock, 5) == -1) error_handling("listen() error");
    printf("Chat server started on port %s\n", argv[1]);

    while (true) {
        struct sockaddr_in clnt_addr;
        socklen_t clnt_addr_size = sizeof(clnt_addr);
        int clnt_sock = accept(serv_sock, (struct sockaddr*)&clnt_addr, &clnt_addr_size);
        if (clnt_sock == -1) {
            if (errno == EINTR) continue;
            error_handling("accept() error");
        }

        pthread_mutex_lock(&state_mutex);
        if (clients.size() >= MAX_CLIENT) {
            pthread_mutex_unlock(&state_mutex);
            send_all(clnt_sock, format_notice_line("server is full."));
            close(clnt_sock);
            continue;
        }
        Client* client = new Client;
        client->sock = clnt_sock;
        client->id = next_client_id++;
        client->username = "";
        client->nickname = "user" + std::to_string(client->id);
        client->profile = "";
        client->status = "online";
        client->profile_public = true;
        client->status_public = true;
        client->room = "lobby";
        client->last_active = std::time(NULL);
        clients.push_back(client);
        pthread_mutex_unlock(&state_mutex);

        pthread_t t_id;
        if (pthread_create(&t_id, NULL, handle_clnt, client) != 0) {
            remove_client(client);
            close(clnt_sock);
            delete client;
            continue;
        }
        pthread_detach(t_id);
    }

    close(serv_sock);
    return 0;
}
