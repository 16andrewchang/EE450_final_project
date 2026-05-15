// serverA.cpp
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <unordered_set>
#define PORT_UDP  21218
#define MAXLINE   1024
#define PORT_serverM 24218

struct Tx { int id; std::string from, to; int amt; };
static std::unordered_set<int> seen_ids;  // dedupe by serial for idempotent retries
std::string shift_encrypt(const std::string &dec) {
    std::string res;
    res.reserve(dec.size());
    for (char c : dec) {
        if (c >= 'a' && c <= 'z') {
            res.push_back(char('a' + (c - 'a' + 3) % 26));
        }
        else if (c >= 'A' && c <= 'Z') {
            res.push_back(char('A' + (c - 'A' + 3) % 26));
        }
        else if (c >= '0' && c <= '9') {
            res.push_back(char('0' + (c - '0' + 3) % 10));
        }
        else {
            res.push_back(c);
        }
    }
    return res;
}
std::string shift_decrypt(const std::string &s) {
    std::string res;
    for (char c : s) {
        if (c >= 'a' && c <= 'z')
            res += char((c - 'a' + 26 - 3) % 26 + 'a');
        else if (c >= 'A' && c <= 'Z')
            res += char((c - 'A' + 26 - 3) % 26 + 'A');
        else if (c >= '0' && c <= '9')
            res += char((c - '0' + 10 - 3) % 10 + '0');
        else
            res += c;
    }
    return res;
}
static const std::string XOR_KEY =
    "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
    "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF";
static std::string xor_encrypt(const std::string &str) {
    std::string out;
    out.reserve(str.size());
    for (size_t i = 0; i < str.size(); ++i)
        out += char(str[i] ^ XOR_KEY[i % XOR_KEY.size()] );
    return out;
}
static std::string xor_decrypt(const std::string &cipher) {
    return xor_encrypt(cipher); 
}
int main() {
    std::ifstream fin("block1.txt");
    if (!fin) { perror("block1.txt open"); exit(1); }
    std::vector<Tx> txs;
    while (!fin.eof()) {
        Tx t;
        if (fin >> t.id >> t.from >> t.to >> t.amt) {
            txs.push_back(t);
            seen_ids.insert(t.id);
        }
    }
    fin.close();

    int sockfd;
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket creation failed"); exit(EXIT_FAILURE);
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family      = AF_INET; // IPv4
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port        = htons(PORT_UDP);

    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind failed"); exit(EXIT_FAILURE);
    }

    printf("The Server A is up and running using UDP on port %d.\n", PORT_UDP);
    struct sockaddr_in mainServAddr;
    memset(&mainServAddr, 0, sizeof(mainServAddr));
    mainServAddr.sin_family = AF_INET;
    mainServAddr.sin_port = htons(PORT_serverM); 
    inet_pton(AF_INET, "127.0.0.1", &mainServAddr.sin_addr);
    while (1) {
        char buffer[MAXLINE];
        socklen_t len = sizeof(mainServAddr);
        int n = recvfrom(sockfd, buffer, MAXLINE, MSG_WAITALL,(struct sockaddr *)&mainServAddr, &len);
        buffer[n] = '\0';
        printf("The ServerA received a request from the Main Server.\n");
        std::istringstream iss(buffer);
        std::vector<std::string> tok;
        std::string s;
        while (iss >> s) tok.push_back(s);
        //handle monitor request
        if (tok.size()==2 && tok[0]=="TXLIST" && tok[1]=="SHIFT") {
    
            std::ostringstream oss;
            for (auto &t : txs) {
                oss << t.id << ' '
                    << t.from << ' '
                    << t.to   << ' '
                    << t.amt  << '\n';
            }
            std::string out = oss.str();
            sendto(sockfd, out.data(), out.size(), 0,(sockaddr*)&mainServAddr, sizeof(mainServAddr));
            printf("The ServerA finished sending the response to the Main Server.\n");
            continue;
        }
        //handle monitor request
        else if(tok.size()==2 && tok[0]=="TXLIST" && tok[1]=="XOR"){
            std::ostringstream oss;
            for (auto &t : txs) {
                oss << t.id << ' '
                    << xor_encrypt(shift_decrypt(t.from)) << ' '
                    << xor_encrypt(shift_decrypt(t.to))   << ' '
                    << xor_encrypt(shift_decrypt(std::to_string(t.amt)))  << '\n';
            }
            std::string out = oss.str();
            sendto(sockfd, out.data(), out.size(), 0,(sockaddr*)&mainServAddr, sizeof(mainServAddr));
            printf("The ServerA finished sending the response to the Main Server.\n");
            continue;
        }
        else if (tok.size() == 1 && tok[0]=="MAXID") {
                int maxId = 0;
                for (auto &t : txs) {
                    if (t.id > maxId) maxId = t.id;
                }
                int32_t netid = htonl(maxId);
                sendto(sockfd, &netid, sizeof(netid), 0,
                       (sockaddr*)&mainServAddr, sizeof(mainServAddr));
                continue;
        }
        //handle client request
        else if ((tok.size() == 2 && tok[0]=="SHIFT") || (tok.size() == 2 && tok[0]=="XOR")) {
            std::string user;
            if(tok[0]=="SHIFT"){
            user = tok[1];
            }
            if(tok[0]=="XOR"){
             std::string dec_user = xor_decrypt(tok[1]);
             user = shift_encrypt(dec_user);

            }
            std::vector<int> response;
            for (auto &t : txs) {
                if (t.from == user) response.push_back(-t.amt);
                if (t.to   == user) response.push_back(t.amt);
            }
            if(tok[0]=="SHIFT"){
                std::string payload;
                payload.reserve(response.size());
                for (size_t i = 0; i < response.size(); i++) {
                payload += std::to_string(response[i]);
                if (i + 1 < response.size()) payload += ' ';
            }
            sendto(sockfd,payload.data(),payload.size(),MSG_CONFIRM,(const struct sockaddr *)&mainServAddr, sizeof(mainServAddr));
            }
            if(tok[0]=="XOR"){
               std::string payload;
               payload.reserve(response.size());
               for (size_t i = 0; i < response.size(); i++) {
                   std::string str = shift_decrypt(std::to_string(response[i]));
                   payload += xor_encrypt(str)+' ';
               }
               sendto(sockfd,payload.data(),payload.size(),MSG_CONFIRM,(const struct sockaddr *)&mainServAddr, sizeof(mainServAddr));
            }
            printf("The ServerA finished sending the response to the Main Server.\n");
        }
        else if(tok.size() == 5){
            int serial;
            std::string sender;
            std::string receiver;
            int amt;
            if(tok[0]=="SHIFT"){
                serial = std::stoi(tok[1]);
                sender = tok[2];
                receiver = tok[3];
                amt = std::stoi(tok[4]);
            }
            if(tok[0]=="XOR"){
                serial = std::stoi(tok[1]);
                std::string dec_sender = xor_decrypt(tok[2]);
                sender = shift_encrypt(dec_sender);
                std::string dec_receiver = xor_decrypt(tok[3]);
                receiver = shift_encrypt(dec_receiver);
                std::string dec_amt = xor_decrypt(tok[4]);
                amt = std::stoi(shift_encrypt(dec_amt));
            }
            // Idempotent: if the coordinator already got an ack but lost it
            // to a UDP timeout and retried, do not double-append.
            if (seen_ids.count(serial)) {
                const char* ack = "LOG_OK";
                sendto(sockfd, ack, strlen(ack), 0,
                       (sockaddr*)&mainServAddr, sizeof(mainServAddr));
            } else {
                std::ofstream fout("block1.txt", std::ios::app);
                if (!fout) {
                    perror("block1.txt append");
                }
                else {
                    fout<< '\n'
                        << serial << ' '
                        << sender << ' '
                        << receiver << ' '
                        << std::to_string(amt);
                    fout.flush();
                    fout.close();
                    txs.push_back(Tx{serial, sender, receiver, amt});
                    seen_ids.insert(serial);
                }
                const char* ack = "LOG_OK";
                sendto(sockfd, ack, strlen(ack), 0,
                       (sockaddr*)&mainServAddr, sizeof(mainServAddr));
            }
        }
        
        else {
            const char *err = "ERROR";
            sendto(sockfd, err, strlen(err), 0,
                   (sockaddr*)&mainServAddr, sizeof(mainServAddr));
            fprintf(stderr, "ServerA: malformed request: %s\n", buffer);
        }

          
    }

    close(sockfd);
    return 0;
}
