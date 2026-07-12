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
#include <set>
#include <sstream>
#include <string>
#include <vector>

#define BUFSIZE 1024
#define MAX_CLIENT 64

struct Client {
    int sock;
    int id;
    std::string nickname;
    std::string profile;
    std::string room;
};

std::vector<Client*> clients;
std::set<std::string> rooms;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
int next_client_id = 1;

void error_handling(const char* message) {
    perror(message);
    exit(1);
}

bool send_all(int sock, const std::string& data) {
    const char* buf = data.c_str();
    size_t total = 0;
    size_t len = data.size();

    while (total < len) {
        ssize_t sent = write(sock, buf + total, len - total);
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

void broadcast_room(const std::string& room, const std::string& message, int except_sock = -1) {
    pthread_mutex_lock(&clients_mutex);
    for (Client* client : clients) {
        if (client->room == room && client->sock != except_sock) {
            send_all(client->sock, message);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

std::string room_list_text() {
    pthread_mutex_lock(&clients_mutex);
    std::ostringstream oss;
    oss << "[방 목록]";
    for (const std::string& room : rooms) {
        oss << " " << room;
    }
    oss << "\n";
    pthread_mutex_unlock(&clients_mutex);
    return oss.str();
}

std::string who_list_text(const std::string& room) {
    pthread_mutex_lock(&clients_mutex);
    std::ostringstream oss;
    oss << "[현재 방 사용자: " << room << "]\n";
    for (Client* client : clients) {
        if (client->room == room) {
            oss << "- " << client->nickname;
            if (!client->profile.empty()) {
                oss << " (" << client->profile << ")";
            }
            oss << "\n";
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return oss.str();
}

void remove_client(Client* target) {
    pthread_mutex_lock(&clients_mutex);
    clients.erase(std::remove(clients.begin(), clients.end(), target), clients.end());
    pthread_mutex_unlock(&clients_mutex);
}

void change_room(Client* client, const std::string& requested_room, bool creating) {
    std::string room = trim_copy(requested_room);
    if (room.empty()) {
        send_to_client(client, format_notice_line("방 이름을 입력하세요. 예: /join study 또는 /create study"));
        return;
    }

    std::string old_room;
    pthread_mutex_lock(&clients_mutex);
    bool existed = rooms.count(room) > 0;
    if (creating) {
        rooms.insert(room);
    }
    if (!creating && !existed) {
        pthread_mutex_unlock(&clients_mutex);
        send_to_client(client, format_notice_line("존재하지 않는 방입니다. /create " + room + " 으로 먼저 만드세요."));
        return;
    }
    old_room = client->room;
    client->room = room;
    pthread_mutex_unlock(&clients_mutex);

    if (creating && !existed) {
        broadcast_room(old_room, format_notice_line(client->nickname + "님이 단체방 '" + room + "'을 만들었습니다."));
    }
    broadcast_room(old_room, format_notice_line(client->nickname + "님이 방을 나갔습니다."), client->sock);
    broadcast_room(room, format_notice_line(client->nickname + "님이 입장했습니다."), client->sock);
    send_to_client(client, format_notice_line("'" + room + "' 방에 입장했습니다."));
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
        send_to_client(client,
            "[명령어]\n"
            "/nick <닉네임>        닉네임 변경\n"
            "/profile <내용>       프로필/상태 메시지 설정\n"
            "/create <방이름>      단체방 만들고 입장\n"
            "/join <방이름>        단체방 입장\n"
            "/rooms                방 목록 보기\n"
            "/who                  현재 방 사용자 보기\n"
            "/msg <메시지>         현재 방에 메시지 전송\n"
            "/quit                 종료\n"
            "명령어 없이 입력해도 현재 방에 메시지가 전송됩니다.\n");
    } else if (command == "/nick") {
        if (arg.empty()) {
            send_to_client(client, format_notice_line("닉네임을 입력하세요. 예: /nick minju"));
            return;
        }
        std::string old_nick;
        std::string room;
        pthread_mutex_lock(&clients_mutex);
        old_nick = client->nickname;
        client->nickname = arg;
        room = client->room;
        pthread_mutex_unlock(&clients_mutex);
        broadcast_room(room, format_notice_line(old_nick + "님이 닉네임을 " + arg + "(으)로 변경했습니다."));
    } else if (command == "/profile") {
        pthread_mutex_lock(&clients_mutex);
        client->profile = arg;
        pthread_mutex_unlock(&clients_mutex);
        send_to_client(client, format_notice_line("프로필이 설정되었습니다: " + (arg.empty() ? "(비어 있음)" : arg)));
    } else if (command == "/create") {
        change_room(client, arg, true);
    } else if (command == "/join") {
        change_room(client, arg, false);
    } else if (command == "/rooms") {
        send_to_client(client, room_list_text());
    } else if (command == "/who") {
        std::string room;
        pthread_mutex_lock(&clients_mutex);
        room = client->room;
        pthread_mutex_unlock(&clients_mutex);
        send_to_client(client, who_list_text(room));
    } else if (command == "/msg") {
        if (!arg.empty()) {
            pthread_mutex_lock(&clients_mutex);
            std::string room = client->room;
            std::string nickname = client->nickname;
            pthread_mutex_unlock(&clients_mutex);
            broadcast_room(room, format_chat_line(room, nickname, arg));
        }
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

    pthread_mutex_lock(&clients_mutex);
    std::string room = client->room;
    std::string nickname = client->nickname;
    pthread_mutex_unlock(&clients_mutex);
    broadcast_room(room, format_chat_line(room, nickname, line));
}

void* handle_clnt(void* arg) {
    Client* client = static_cast<Client*>(arg);
    char buffer[BUFSIZE];
    std::string pending;

    send_to_client(client,
        format_notice_line("채팅 서버에 연결되었습니다. 기본 방은 lobby 입니다. /help 로 명령어를 확인하세요."));
    broadcast_room(client->room, format_notice_line(client->nickname + "님이 입장했습니다."), client->sock);

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

    pthread_mutex_lock(&clients_mutex);
    std::string nickname = client->nickname;
    std::string room = client->room;
    pthread_mutex_unlock(&clients_mutex);

    remove_client(client);
    broadcast_room(room, format_notice_line(nickname + "님이 연결을 종료했습니다."));
    close(client->sock);
    delete client;
    return NULL;
}

int main(int argc, char* argv[]) {
    int serv_sock;
    struct sockaddr_in serv_addr;

    if (argc != 2) {
        printf("Usage : %s <port>\n", argv[0]);
        exit(1);
    }

    rooms.insert("lobby");

    serv_sock = socket(PF_INET, SOCK_STREAM, 0);
    if (serv_sock == -1) {
        error_handling("socket() error");
    }

    int option = 1;
    setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

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

    printf("Chat server started on port %s\n", argv[1]);

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

        pthread_mutex_lock(&clients_mutex);
        if (clients.size() >= MAX_CLIENT) {
            pthread_mutex_unlock(&clients_mutex);
            send_all(clnt_sock, format_notice_line("서버 접속 인원이 가득 찼습니다."));
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
        pthread_mutex_unlock(&clients_mutex);

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
