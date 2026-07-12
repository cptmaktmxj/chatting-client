//표준 헤더(운영체제 상관없이 사용 가능)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
//리눅스 헤더
#include <unistd.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>

#define BUFSIZE 1024
#define MAX_CLIENT 64

int clnt_socks[MAX_CLIENT];
int clnt_id=0;

void error_handling(const char* message){
    fputs(message, stderr);
    fputc("\n", stderr);
    exit(-1);
}

void* handle_clnt(void* arg){ //멀티스레드 서버의 동작

    int clnt_sock = *((int*)arg); //clnt_sock = 클라이언트 식별번호
    int str_len = 0;
    char msg[BUFSIZE] = {0,};
    int idx;

    while(0 != (str_len = read(clnt_sock, msg, sizeof(msg)))){
        int idy = 0;

        while(idy <= clnt_id){
            if (clnt_sock != clnt_socks[idy]){ //메시지를 보낸 클라이언트 제외한 모든 클라이언트에게 메시지 전송
                write(clnt_socks[idy], msg, str_len);
            }
            idy++;
        }
    }

    for(idx=0; idx < clnt_id; idx++){
        if (clnt_sock == clnt_socks[idx]){ 
            while(idx++ < clnt_id -1){
                clnt_socks[idx] = clnt_socks[idx+1]; //연결이 끊긴 사용자 자리 채우기
                break;
            }
        }
    }

    clnt_id--;
    close(clnt_sock);
    return NULL;
}

int main(int argc, char* argv[]){

    int serv_sock;
    int clnt_sock;
    pthread_t t_id;

    struct sockaddr_in serv_addr;
    struct sockaddr_in clnt_addr;
    unsigned int clnt_addr_size;

    if (2 != argc){
        printf("Usage : %s <port>\n", argv[0]);
        exit(1);
    }

    //SOCK_STREAM = TCP, SOCK_DGRAM = UDP
    serv_sock = socket(PF_INET, SOCK_STREAM, 0);
    

    if (-1 == serv_sock){
        error_handling("socket() error");
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    //주소체계, IP, 포트 할당
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl("INADDR_ANY"); // htonl = host to network, INADDR_ANY = 자신의 ip 주소
    serv_addr.sin_port = htons(atoi(argv[1]));

    if (-1 == bind(serv_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr))){
        error_handling("bind() error");
    }

    if (-1 == listen(serv_sock, 5)){
        error_handling("listen() error");
    }

    clnt_addr_size = sizeof(clnt_addr);

    while(1){
        clnt_sock = accept(serv_sock, (struct sockaddr*)&clnt_addr, &clnt_addr_size);
        
        if (-1 == clnt_sock){
            error_handling("accept() error");
        }

        clnt_socks[clnt_id++] = clnt_sock;

        pthread_create(&t_id, NULL, handle_clnt, (void*)clnt_sock); //t_id : thread id, handle_clnt : 스레드 풀을 만드는 함수, clnt_sock : 특정 클라이언트에 대한 실행

    }

    return 0;
}