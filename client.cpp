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
#define MAIN_SERVER_PORT     (25000 + USC_ID) 
#define MAXLINE          1024

int main(int argc, char** argv) {
    
    if (!((argc==2) || (argc==3 && strcmp(argv[2],"XOR")==0) || (argc==4) || (argc==5 && strcmp(argv[4],"XOR")==0))) {
        fprintf(stderr, "Usage: %s <username> [<receiver> <amount>] [XOR]\n", argv[0]);
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
    printf("The client is up and running.\n");
    char request[MAXLINE];
    if (argc == 2|| (argc==3 && strcmp(argv[2],"XOR")==0)) {
        const char* user = argv[1];
        if (argc==3){
            snprintf(request, sizeof(request), "CHECK %s %s", user, argv[2]);
        }
        else{
            snprintf(request, sizeof(request), "CHECK %s", user);
        }
        printf("%s sent a balance enquiry request to the main server.\n", user);
    }
    else{
        const char* sender   = argv[1];
        const char* receiver = argv[2];
        const char* amount   = argv[3]; 
        if (argc==5 && strcmp(argv[4],"XOR")==0){
            snprintf(request, sizeof(request),"TX %s %s %s %s",sender, receiver, amount, argv[4]);
        } 
        else{
        snprintf(request, sizeof(request), "TX %s %s %s", sender, receiver, amount);
        }
        printf("%s has requested to transfer %s txcoins to %s.\n",sender, amount, receiver);
    }

    send(sock, request, strlen(request), 0);
    char buffer[MAXLINE];
    int n = recv(sock, buffer, MAXLINE, 0);
    if (n <= 0) {
        close(sock);
        return 0;
    }
    buffer[n] = '\0';
    std::istringstream iss(buffer);
    std::vector<std::string> tok;
    std::string s;
    while (iss >> s) tok.push_back(s);

    if (argc == 2 || (argc==3 && strcmp(argv[2],"XOR")==0)) {
        const char* user = argv[1];
        if (tok.size() == 2 && tok[0] == "BALANCE") {
            printf("The current balance of %s is : %s txcoins.\n",user, tok[1].c_str());
        }
        else if (tok.size() == 2 && tok[0] == "ERROR_USER") {
            printf("%s is not a part of the network.\n", user);
        }
        else {
            printf("Unexpected reply: %s\n", buffer);
        }
    }
    else{
        const char* sender   = argv[1];
        const char* receiver = argv[2];
        const char* amount   = argv[3];
        if (tok.size() == 2 && tok[0] == "SUCCESS") {
            // "SUCCESS <new_balance>"
            printf("%s successfully transferred %s txcoins to %s.\n",
                   sender, amount, receiver);
            printf("The current balance of %s is : %s txcoins.\n",
                   sender, tok[1].c_str());
        }
        else if (tok.size() == 2 && tok[0] == "INSUFFICIENT") {
            // "FAIL_INSUFFICIENT <balance>"
            printf("%s was unable to transfer %s txcoins to %s because of insufficient balance .\n",
                   sender, amount, receiver);
            printf("The current balance of %s is : %s txcoins.\n",
                   sender, tok[1].c_str());
        }
        else if (tok.size() == 2 && tok[0] == "ERROR_USER") {
            // "ERROR_USER <bad_username>"
            printf("Unable to proceed with the transaction as %s is not part of the network.\n",
                   tok[1].c_str());
        }
        else if (tok.size() == 3 && tok[0] == "ERROR_USERS") {
            // "ERROR_USERS <u1> <u2>"
            printf("Unable to proceed with the transaction as %s and %s are not part of the network.\n",
                   tok[1].c_str(), tok[2].c_str());
        }
        else {
            // unexpected
            printf("Unexpected reply: %s\n", buffer);
        }
    }
    close(sock);
    return 0;

}