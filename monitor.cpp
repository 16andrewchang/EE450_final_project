#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sstream>
#include <vector>

#define USC_ID           218
#define MAIN_SERVER_PORT     (26000 + USC_ID) 
#define MAXLINE          1024

int main(int argc, char** argv) {
    if (!((argc == 2 && std::string(argv[1]) == "TXLIST")|| (argc == 3 && std::string(argv[1]) == "TXLIST" && std::string(argv[2]) == "XOR"))){
        fprintf(stderr, "Usage: %s TXLIST [XOR]\n", argv[0]);
        return 1;
    }
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    sockaddr_in servAddr{};
    servAddr.sin_family = AF_INET;
    servAddr.sin_port   = htons(MAIN_SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &servAddr.sin_addr);

    if (connect(sock, (sockaddr*)&servAddr, sizeof(servAddr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    printf("The monitor is up and running.\n");
    char req[MAXLINE];
    if(argc==3 && strcmp(argv[2],"XOR")==0){
        snprintf(req, sizeof(req), "TXLIST %s",argv[2]);
    }
    else{
        snprintf(req, sizeof(req), "TXLIST");
    }
    send(sock, req, strlen(req), 0);
    printf("Monitor sent a sorted list request to the main server.\n");
    
    char buffer[MAXLINE];
    int n = recv(sock, buffer, MAXLINE, 0);
    if (n > 0) {
        buffer[n] = '\0';
        if (std::string(buffer) == "TXLIST_OK") {
            printf("Successfully received confirmation that txchain.txt was created on the main server.\n");
        } 
        else {
            printf("Unexpected reply from main server: %s\n", buffer);
        }
    }
    close(sock);
    return 0;
    
    
}