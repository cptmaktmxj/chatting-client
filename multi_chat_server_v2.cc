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
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#define BUFSIZE 1024
#define MAX_CLIENT 64

enum RoomType {
    ROOM_NORMAL,
    ROOM_HIDDEN,
    ROOM_ANNOUNCE,
    ROOM_APPROVAL,
    ROOM_SECURE,
    ROOM_PRIVATE
};

struct Room {
    std::string name;
    RoomType type;
    std::string owner;
    std::string password_hash;
    std::set<std::string> admins;
    std::set<std::string> permitted;
};

struct Client {
    int sock;
    int id;
    std::string nickname;
    std::string profile;
    std::string room;
};

std::vector<Client*> clients;
std::map<std::string, Room> rooms;
std::map<std::string, std::string> open_links;
pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
int next_client_id = 1;
std::string state_path = "chat_state.db";

void error_handling(const char* message) {
    perror(message);
    exit(1);
}

std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> out;
    std::string item;
    std::istringstream iss(text);
    while (std::getline(iss, item, delimiter)) {
        out.push_back(item);
    }
    return out;
}

std::string join_set(const std::set<std::string>& values) {
    std::ostringstream oss;
    bool first = true;
    for (const std::string& value : values) {
        if (!first) {
            oss << ",";
        }
        oss << value;
        first = false;
    }
    return oss.str();
}

std::set<std::string> parse_set(const std::string& text) {
    std::set<std::string> values;
    for (const std::string& item : split(text, ',')) {
        std::string value = trim_copy(item);
        if (!value.empty()) {
            values.insert(value);
        }
    }
    return values;
}

std::string room_type_to_string(RoomType type) {
    switch (type) {
        case ROOM_HIDDEN: return "hidden";
        case ROOM_ANNOUNCE: return "announce";
        case ROOM_APPROVAL: return "approval";
        case ROOM_SECURE: return "secure";
        case ROOM_PRIVATE: return "private";
        case ROOM_NORMAL:
        default: return "normal";
    }
}

RoomType parse_room_type(const std::string& type) {
    if (type == "hidden") return ROOM_HIDDEN;
    if (type == "announce") return ROOM_ANNOUNCE;
    if (type == "approval") return ROOM_APPROVAL;
    if (type == "secure") return ROOM_SECURE;
    if (type == "private") return ROOM_PRIVATE;
    return ROOM_NORMAL;
}

std::string hash_password(const std::string& password) {
    std::hash<std::string> hasher;
    size_t value = hasher("chat-room-password:" + password);
    std::ostringstream oss;
    oss << std::hex << value;
    return oss.str();
}

bool send_all(int sock, const std::string& data) {
    const char* buf = data.c_str();
    size_t total = 0;
    while (total < data.size()) {
        ssize_t sent = write(sock, buf + total, data.size() - total);
        if (sent <= 0) {
            return false;
        }
        total += static_cast<size_t>(sent);
    }
    return true;
}

void send_to_client(Client* client, const std::string& message) {
    if (client != NULL) {
        send_all(client->sock, message);
    }
}

Room make_room(const std::string& name, RoomType type, const std::string& owner, const std::string& password = "") {
    Room room;
    room.name = name;
    room.type = type;
    room.owner = owner;
    room.password_hash = password.empty() ? "" : hash_password(password);
    room.admins.insert(owner);
    room.permitted.insert(owner);
    return room;
}

void save_state_unlocked() {
    std::ofstream out(state_path.c_str(), std::ios::trunc);
    for (const auto& pair : rooms) {
        const Room& room = pair.second;
        out << "ROOM|"
            << room.name << "|"
            << room_type_to_string(room.type) << "|"
            << room.owner << "|"
            << room.password_hash << "|"
            << join_set(room.admins) << "|"
            << join_set(room.permitted) << "\n";
    }
    for (const auto& pair : open_links) {
        out << "LINK|" << pair.first << "|" << pair.second << "\n";
    }
}

void save_state() {
    pthread_mutex_lock(&state_mutex);
    save_state_unlocked();
    pthread_mutex_unlock(&state_mutex);
}

void load_state() {
    pthread_mutex_lock(&state_mutex);
    rooms.clear();
    open_links.clear();

    std::ifstream in(state_path.c_str());
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> parts = split(line, '|');
        if (parts.size() >= 7 && parts[0] == "ROOM") {
            Room room;
            room.name = parts[1];
            room.type = parse_room_type(parts[2]);
            room.owner = parts[3];
            room.password_hash = parts[4];
            room.admins = parse_set(parts[5]);
            room.permitted = parse_set(parts[6]);
            if (room.admins.empty() && !room.owner.empty()) {
                room.admins.insert(room.owner);
            }
            rooms[room.name] = room;
        } else if (parts.size() >= 3 && parts[0] == "LINK") {
            open_links[parts[1]] = parts[2];
        }
    }

    if (rooms.count("lobby") == 0) {
        rooms["lobby"] = make_room("lobby", ROOM_NORMAL, "server");
    }
    save_state_unlocked();
    pthread_mutex_unlock(&state_mutex);
}

bool is_admin_unlocked(const Room& room, const std::string& nickname) {
    return room.admins.count(nickname) > 0;
}

bool can_speak_unlocked(const Room& room, const std::string& nickname) {
    if (is_admin_unlocked(room, nickname)) {
        return true;
    }
    if (room.type == ROOM_ANNOUNCE) {
        return false;
    }
    if (room.type == ROOM_APPROVAL) {
        return room.permitted.count(nickname) > 0;
    }
    return true;
}

std::string display_name_unlocked(const Room& room, const std::string& nickname) {
    if (room.type == ROOM_HIDDEN) {
        return "anonymous";
    }
    return nickname;
}

Client* find_client_by_nick_unlocked(const std::string& nickname) {
    for (Client* client : clients) {
        if (client->nickname == nickname) {
            return client;
        }
    }
    return NULL;
}

void broadcast_room(const std::string& room_name, const std::string& message, int except_sock = -1) {
    pthread_mutex_lock(&state_mutex);
    for (Client* client : clients) {
        if (client->room == room_name && client->sock != except_sock) {
            send_all(client->sock, message);
        }
    }
    pthread_mutex_unlock(&state_mutex);
}

void broadcast_private_room(const std::string& room_name,
                            const std::string& owner,
                            const std::string& message,
                            int except_sock = -1) {
    pthread_mutex_lock(&state_mutex);
    for (Client* client : clients) {
        if ((client->room == room_name || client->nickname == owner) && client->sock != except_sock) {
            send_all(client->sock, message);
        }
    }
    pthread_mutex_unlock(&state_mutex);
}

void remove_client(Client* target) {
    pthread_mutex_lock(&state_mutex);
    clients.erase(std::remove(clients.begin(), clients.end(), target), clients.end());
    pthread_mutex_unlock(&state_mutex);
}

std::string room_list_text() {
    pthread_mutex_lock(&state_mutex);
    std::ostringstream oss;
    oss << "[rooms]";
    for (const auto& pair : rooms) {
        oss << " " << pair.first << "(" << room_type_to_string(pair.second.type) << ")";
    }
    if (!open_links.empty()) {
        oss << " [openlinks]";
        for (const auto& pair : open_links) {
            oss << " " << pair.first << "->" << pair.second;
        }
    }
    oss << "\n";
    pthread_mutex_unlock(&state_mutex);
    return oss.str();
}

std::string who_list_text(const std::string& room_name) {
    pthread_mutex_lock(&state_mutex);
    std::ostringstream oss;
    oss << "[users " << room_name << "]\n";
    for (Client* client : clients) {
        if (client->room == room_name) {
            oss << "- " << client->nickname;
            if (!client->profile.empty()) {
                oss << " (" << client->profile << ")";
            }
            oss << "\n";
        }
    }
    pthread_mutex_unlock(&state_mutex);
    return oss.str();
}

void join_room(Client* client, const std::string& room_name) {
    std::string old_room;
    std::string nickname;
    pthread_mutex_lock(&state_mutex);
    old_room = client->room;
    nickname = client->nickname;
    client->room = room_name;
    pthread_mutex_unlock(&state_mutex);

    broadcast_room(old_room, format_notice_line(nickname + " left the room."), client->sock);
    broadcast_room(room_name, format_notice_line(nickname + " joined the room."), client->sock);
    send_to_client(client, format_notice_line("joined " + room_name));
}

bool require_admin(Client* client, Room& room) {
    if (!is_admin_unlocked(room, client->nickname)) {
        send_to_client(client, format_notice_line("관리자 권한이 필요합니다."));
        return false;
    }
    return true;
}

void create_room_command(Client* client, const std::string& name, RoomType type, const std::string& password = "") {
    std::string room_name = trim_copy(name);
    if (room_name.empty()) {
        send_to_client(client, format_notice_line("방 이름을 입력하세요."));
        return;
    }
    if (type == ROOM_SECURE && password.empty()) {
        send_to_client(client, format_notice_line("보안방 비밀번호를 입력하세요."));
        return;
    }

    pthread_mutex_lock(&state_mutex);
    if (rooms.count(room_name) == 0) {
        rooms[room_name] = make_room(room_name, type, client->nickname, password);
        save_state_unlocked();
    }
    pthread_mutex_unlock(&state_mutex);

    join_room(client, room_name);
}

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
    Room room = it->second;
    if (room.type == ROOM_SECURE && room.password_hash != hash_password(password)) {
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("비밀번호가 틀렸습니다."));
        return;
    }
    if (room.type == ROOM_SECURE) {
        rooms[name].permitted.insert(client->nickname);
        save_state_unlocked();
    }
    pthread_mutex_unlock(&state_mutex);

    join_room(client, name);
}

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

void handle_enterlink(Client* client, const std::string& link_name) {
    std::string link = trim_copy(link_name);
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
    room_name = link + "_" + client->nickname;
    if (rooms.count(room_name) == 0) {
        Room room = make_room(room_name, ROOM_PRIVATE, owner);
        room.permitted.insert(client->nickname);
        rooms[room_name] = room;
        save_state_unlocked();
    }
    pthread_mutex_unlock(&state_mutex);

    join_room(client, room_name);
}

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

    if (command == "/permit") {
        room.permitted.insert(target);
        save_state_unlocked();
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line(target + " 말하기 권한 허가"));
    } else if (command == "/revoke") {
        room.permitted.erase(target);
        save_state_unlocked();
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line(target + " 말하기 권한 회수"));
    } else if (command == "/delegate") {
        room.admins.insert(target);
        room.permitted.insert(target);
        save_state_unlocked();
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line(target + " 관리자 위임"));
    } else if (command == "/kick") {
        Client* target_client = find_client_by_nick_unlocked(target);
        if (target_client != NULL && target_client->room == client->room) {
            target_client->room = "lobby";
            send_all(target_client->sock, format_notice_line("강퇴되어 lobby로 이동했습니다."));
        }
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line(target + " 강퇴 처리"));
    } else {
        pthread_mutex_unlock(&state_mutex);
    }
}

void handle_roominfo(Client* client) {
    pthread_mutex_lock(&state_mutex);
    Room room = rooms[client->room];
    std::ostringstream oss;
    oss << "[roominfo] " << room.name
        << " type=" << room_type_to_string(room.type)
        << " owner=" << room.owner
        << " admins=" << join_set(room.admins)
        << " permitted=" << join_set(room.permitted)
        << "\n";
    pthread_mutex_unlock(&state_mutex);
    send_to_client(client, oss.str());
}

void send_help(Client* client) {
    send_to_client(client,
        "[commands]\n"
        "/nick <name>\n"
        "/profile <text>\n"
        "/create <room>\n"
        "/create_hidden <room>\n"
        "/create_announce <room>\n"
        "/create_approval <room>\n"
        "/create_secure <room> <password>\n"
        "/join <room>\n"
        "/join_secure <room> <password>\n"
        "/openlink <name>\n"
        "/enterlink <name>\n"
        "/permit <nick>\n"
        "/revoke <nick>\n"
        "/delegate <nick>\n"
        "/kick <nick>\n"
        "/roominfo\n"
        "/rooms\n"
        "/who\n"
        "/quit\n");
}

void handle_command(Client* client, const std::string& line) {
    std::string command;
    std::string arg;
    size_t space_pos = line.find(' ');
    if (space_pos == std::string::npos) {
        command = line;
    } else {
        command = line.substr(0, space_pos);
        arg = trim_copy(line.substr(space_pos + 1));
    }

    if (command == "/help") {
        send_help(client);
    } else if (command == "/nick") {
        if (arg.empty()) {
            send_to_client(client, format_notice_line("닉네임을 입력하세요."));
            return;
        }
        pthread_mutex_lock(&state_mutex);
        std::string old = client->nickname;
        client->nickname = arg;
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line(old + " -> " + arg));
    } else if (command == "/profile") {
        pthread_mutex_lock(&state_mutex);
        client->profile = arg;
        pthread_mutex_unlock(&state_mutex);
        send_to_client(client, format_notice_line("프로필이 설정되었습니다: " + arg));
    } else if (command == "/create") {
        create_room_command(client, arg, ROOM_NORMAL);
    } else if (command == "/create_hidden") {
        create_room_command(client, arg, ROOM_HIDDEN);
    } else if (command == "/create_announce") {
        create_room_command(client, arg, ROOM_ANNOUNCE);
    } else if (command == "/create_approval") {
        create_room_command(client, arg, ROOM_APPROVAL);
    } else if (command == "/create_secure") {
        std::vector<std::string> parts = split(arg, ' ');
        if (parts.size() < 2) {
            send_to_client(client, format_notice_line("사용법: /create_secure <방이름> <비밀번호>"));
        } else {
            create_room_command(client, parts[0], ROOM_SECURE, parts[1]);
        }
    } else if (command == "/join") {
        handle_join(client, arg);
    } else if (command == "/join_secure") {
        std::vector<std::string> parts = split(arg, ' ');
        if (parts.size() < 2) {
            send_to_client(client, format_notice_line("사용법: /join_secure <방이름> <비밀번호>"));
        } else {
            handle_join(client, parts[0], parts[1]);
        }
    } else if (command == "/openlink") {
        handle_openlink(client, arg);
    } else if (command == "/enterlink") {
        handle_enterlink(client, arg);
    } else if (command == "/permit" || command == "/revoke" || command == "/delegate" || command == "/kick") {
        handle_permission_command(client, command, arg);
    } else if (command == "/roominfo") {
        handle_roominfo(client);
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

void handle_line(Client* client, const std::string& raw_line) {
    std::string line = trim_copy(raw_line);
    if (line.empty()) {
        return;
    }
    if (line[0] == '/') {
        handle_command(client, line);
        return;
    }

    std::string room_name;
    std::string nickname;
    std::string owner;
    RoomType room_type;
    bool allowed;
    pthread_mutex_lock(&state_mutex);
    room_name = client->room;
    nickname = client->nickname;
    Room room = rooms[room_name];
    owner = room.owner;
    room_type = room.type;
    allowed = can_speak_unlocked(room, nickname);
    std::string shown_name = display_name_unlocked(room, nickname);
    pthread_mutex_unlock(&state_mutex);

    if (!allowed) {
        send_to_client(client, format_notice_line("말할 권한이 없습니다."));
        return;
    }

    std::string message = format_chat_line(room_name, shown_name, line);
    if (room_type == ROOM_PRIVATE) {
        broadcast_private_room(room_name, owner, message);
    } else {
        broadcast_room(room_name, message);
    }
}

void* handle_clnt(void* arg) {
    Client* client = static_cast<Client*>(arg);
    char buffer[BUFSIZE];
    std::string pending;

    send_to_client(client, format_notice_line("connected. default room is lobby. /help"));
    broadcast_room("lobby", format_notice_line(client->nickname + " joined."), client->sock);

    while (true) {
        ssize_t read_len = read(client->sock, buffer, sizeof(buffer));
        if (read_len <= 0) {
            break;
        }
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

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        printf("Usage : %s <port> [state_file]\n", argv[0]);
        exit(1);
    }
    if (argc == 3) {
        state_path = argv[2];
    }
    load_state();

    int serv_sock = socket(PF_INET, SOCK_STREAM, 0);
    if (serv_sock == -1) {
        error_handling("socket() error");
    }

    int option = 1;
    setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(atoi(argv[1]));

    if (bind(serv_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1) {
        error_handling("bind() error");
    }
    if (listen(serv_sock, 5) == -1) {
        error_handling("listen() error");
    }

    printf("Chat server v2 started on port %s\n", argv[1]);

    while (true) {
        struct sockaddr_in clnt_addr;
        socklen_t clnt_addr_size = sizeof(clnt_addr);
        int clnt_sock = accept(serv_sock, (struct sockaddr*)&clnt_addr, &clnt_addr_size);
        if (clnt_sock == -1) {
            if (errno == EINTR) {
                continue;
            }
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
        client->nickname = "user" + std::to_string(client->id);
        client->profile = "";
        client->room = "lobby";
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
