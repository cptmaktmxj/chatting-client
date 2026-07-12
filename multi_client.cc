#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <iostream>
#include <string>

#define BUFSIZE 1024

std::atomic<bool> running(true);

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

void* recv_msg(void* arg) {
    int sock = *static_cast<int*>(arg);
    char msg[BUFSIZE + 1];

    while (running.load()) {
        ssize_t str_len = read(sock, msg, BUFSIZE);
        if (str_len <= 0) {
            break;
        }
        msg[str_len] = '\0';
        std::cout << msg << std::flush;
    }

    running.store(false);
    return NULL;
}

void print_usage() {
    std::cout
        << "클라이언트 명령어\n"
        << "  /nick <닉네임>     닉네임 설정\n"
        << "  /profile <내용>    프로필/상태 메시지 설정\n"
        << "  /create <방이름>   단체방 만들기\n"
        << "  /join <방이름>     단체방 입장\n"
        << "  /rooms             방 목록\n"
        << "  /who               현재 방 사용자\n"
        << "  /quit              종료\n"
        << "명령어 없이 메시지를 입력하면 현재 방에 전송됩니다.\n";
}

int main(int argc, char* argv[]) {
    int sock;
    struct sockaddr_in serv_addr;

    if (argc != 3) {
        printf("Usage : %s <IP> <port>\n", argv[0]);
        exit(1);
    }

    sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        error_handling("socket() error");
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(argv[1]);
    serv_addr.sin_port = htons(atoi(argv[2]));

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1) {
        error_handling("connect() error");
    }

    print_usage();

    pthread_t recv_thread;
    if (pthread_create(&recv_thread, NULL, recv_msg, &sock) != 0) {
        error_handling("pthread_create() error");
    }

    std::string line;
    while (running.load() && std::getline(std::cin, line)) {
        line += "\n";
        if (!send_all(sock, line)) {
            break;
        }
        if (line == "/quit\n") {
            break;
        }
    }

    running.store(false);
    shutdown(sock, SHUT_RDWR);
    pthread_join(recv_thread, NULL);
    close(sock);
    return 0;
}
