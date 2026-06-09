#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <fstream>
#include <thread>
#include <mutex>
#include <unordered_set>
#include <atomic>
#define USC_ID 218
#define PORT_CLIENT_TCP (25000 + USC_ID)
#define PORT_MONITOR_TCP (26000 + USC_ID)
#define MAXLINE 1024
#define PORT_serverA 21218
#define PORT_serverB 22218
#define PORT_serverC 23218
#define PORT_serverM 24218

#define UDP_TIMEOUT_MS 500
#define UDP_MAX_RETRIES 3

int udpSock = -1;
std::mutex udpMtx; // serializes one request/response cycle on udpSock

struct Tx
{
    int id;
    std::string from, to;
    int amt;
};
static const std::string XOR_KEY =
    "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
    "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF";

static std::string xor_encrypt(const std::string &plain)
{
    std::string str;
    str.reserve(plain.size());
    for (size_t i = 0; i < plain.size(); ++i)
    {
        str.push_back(plain[i] ^ XOR_KEY[i % XOR_KEY.size()]);
    }
    return str;
}
static inline std::string xor_decrypt(const std::string &cipher)
{
    return xor_encrypt(cipher);
}
int listener(uint16_t port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("socket");
        exit(1);
    }
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("setsockopt(SO_REUSEADDR) failed");
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sock, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        exit(1);
    }
    if (listen(sock, 16) < 0)
    {
        perror("listen");
        exit(1);
    }
    return sock;
}
std::string encrypt(const std::string &dec)
{
    std::string res;
    res.reserve(dec.size());
    for (char c : dec)
    {
        if (c >= 'a' && c <= 'z')
        {
            res.push_back(char('a' + (c - 'a' + 3) % 26));
        }
        else if (c >= 'A' && c <= 'Z')
        {
            res.push_back(char('A' + (c - 'A' + 3) % 26));
        }
        else if (c >= '0' && c <= '9')
        {
            res.push_back(char('0' + (c - '0' + 3) % 10));
        }
        else
        {
            res.push_back(c);
        }
    }
    return res;
}

std::string decrypt(const std::string &enc)
{
    std::string res;
    res.reserve(enc.size());
    for (char c : enc)
    {
        if (c >= 'a' && c <= 'z')
        {
            char x = char('a' + ((c - 'a' + 26) - 3) % 26);
            res.push_back(x);
        }
        else if (c >= 'A' && c <= 'Z')
        {
            char x = char('A' + ((c - 'A' + 26) - 3) % 26);
            res.push_back(x);
        }
        else if (c >= '0' && c <= '9')
        {
            char x = char('0' + ((c - '0' + 10) - 3) % 10);
            res.push_back(x);
        }
        else
        {
            res.push_back(c);
        }
    }
    return res;
}
static std::string encrypt_generic(const std::string &str, bool xor_flag)
{
    return xor_flag ? xor_encrypt(str) : encrypt(str);
}
static std::string decrypt_generic(const std::string &str, bool xor_flag)
{
    return xor_flag ? xor_decrypt(str) : decrypt(str);
}

// One request/response cycle with timeout + bounded retries. Returns bytes
// received, or -1 on timeout/failure. Caller holds udpMtx.
static int udp_rpc(const void *req, size_t reqLen, int port,
                   char *resp, size_t respCap)
{
    sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serv.sin_addr);

    for (int attempt = 0; attempt < UDP_MAX_RETRIES; ++attempt)
    {
        sendto(udpSock, req, reqLen, 0, (sockaddr *)&serv, sizeof(serv));
        socklen_t len = sizeof(serv);
        int n = recvfrom(udpSock, resp, respCap - 1, 0,
                         (sockaddr *)&serv, &len);
        if (n >= 0)
        {
            resp[n] = '\0';
            return n;
        }
        // EAGAIN/EWOULDBLOCK from SO_RCVTIMEO: peer may be partitioned, retry.
        fprintf(stderr,
                "udp_rpc: timeout on port %d (attempt %d/%d)\n",
                port, attempt + 1, UDP_MAX_RETRIES);
    }
    return -1;
}

int query_balance(const char *user, int port, bool xor_flag)
{
    std::string encrypted_user = encrypt_generic(user, xor_flag);
    std::string mode = xor_flag ? "XOR" : "SHIFT";
    std::string request = mode + " " + encrypted_user;
    char buffer[MAXLINE];

    std::lock_guard<std::mutex> lk(udpMtx);
    int n = udp_rpc(request.data(), request.size(), port, buffer, sizeof(buffer));
    if (n < 0)
    {
        // Backend partitioned: treat slice as 0 so the rest of the system
        // can still answer (degraded consistency, not stale crash).
        fprintf(stderr, "query_balance: backend on port %d unreachable\n", port);
        return 0;
    }
    std::istringstream iss(buffer);
    std::string token;
    int sum = 0;
    while (iss >> token)
    {
        std::string amt = decrypt_generic(token, xor_flag);
        sum += std::stoi(amt);
    }
    return sum;
}
int query_maxid(int port)
{
    const char *cmd = "MAXID";
    char buf[MAXLINE];

    std::lock_guard<std::mutex> lk(udpMtx);
    int n = udp_rpc(cmd, strlen(cmd), port, buf, sizeof(buf));
    if (n < (int)sizeof(int32_t))
        return 0;
    int32_t netid;
    memcpy(&netid, buf, sizeof(netid));
    return ntohl(netid);
}
std::vector<Tx> query_txlist(int port, bool xor_flag)
{
    const char *req = xor_flag ? "TXLIST XOR" : "TXLIST SHIFT";
    char buf[10 * 1024];

    std::lock_guard<std::mutex> lk(udpMtx);
    int n = udp_rpc(req, strlen(req), port, buf, sizeof(buf));
    if (n < 0)
    {
        fprintf(stderr, "query_txlist: backend on port %d unreachable\n", port);
        return {};
    }
    std::istringstream iss(buf);
    std::vector<Tx> tx;
    Tx t;
    while (iss >> t.id >> t.from >> t.to >> t.amt)
    {
        tx.push_back(t);
    }
    return tx;
}

// Send a TX commit to a specific backend. Returns true on LOG_OK.
// Caller holds udpMtx.
static bool send_tx_locked(int port, const std::string &payload)
{
    char buf[MAXLINE];
    int n = udp_rpc(payload.data(), payload.size(), port, buf, sizeof(buf));
    if (n < 0)
        return false;
    return std::string(buf, n) == "LOG_OK";
}

// Try backends in order until one acks. Returns chosen port id ('A'/'B'/'C')
// or 0 on total failure. Skips backends listed in `tried`.
static char commit_tx(int serial,
                      const std::string &sender,
                      const std::string &receiver,
                      const std::string &amount,
                      bool use_xor,
                      const std::unordered_set<char> &tried_init)
{
    std::string mode = use_xor ? "XOR" : "SHIFT";
    std::string e_sender = encrypt_generic(sender, use_xor);
    std::string e_receiver = encrypt_generic(receiver, use_xor);
    std::string e_amount = encrypt_generic(amount, use_xor);
    std::string payload =
        mode + " " +
        std::to_string(serial) + " " +
        e_sender + " " + e_receiver + " " + e_amount;

    int ports[3] = {PORT_serverA, PORT_serverB, PORT_serverC};
    char ids[3] = {'A', 'B', 'C'};

    // Randomize order so load spreads, but always retry every backend.
    int order[3] = {0, 1, 2};
    int start = std::rand() % 3;
    std::rotate(order, order + start, order + 3);

    std::unordered_set<char> tried = tried_init;
    for (int i : order)
    {
        if (tried.count(ids[i]))
            continue;
        printf("The main server sent a request to server %c\n", ids[i]);

        std::lock_guard<std::mutex> lk(udpMtx);
        bool ok = send_tx_locked(ports[i], payload);
        if (ok)
        {
            printf("The main server received the feedback from Server %c using UDP over %d\n",
                   ids[i], ports[i]);
            return ids[i];
        }
        fprintf(stderr, "commit_tx: server %c unresponsive, failing over\n", ids[i]);
        tried.insert(ids[i]);
    }
    return 0;
}

void handle_client_request(int clientSock)
{
    char buffer[1024];
    int n = recv(clientSock, buffer, sizeof(buffer), 0);
    if (n <= 0)
        return;
    buffer[n] = '\0';

    std::istringstream iss(buffer);
    std::vector<std::string> tok;
    std::string s;
    while (iss >> s)
        tok.push_back(s);

    if ((tok.size() == 2 && tok[0] == "CHECK") || (tok.size() == 3 && tok[0] == "CHECK" && tok[2] == "XOR"))
    {
        int currentBalance = 1000;
        const char *user = tok[1].c_str();
        bool use_xor = (tok.size() == 3);
        printf("The main server received input=%s from the client using TCP over %d.\n", user, PORT_CLIENT_TCP);
        printf("The main server sent a request to server A\n");
        int decA = query_balance(user, PORT_serverA, use_xor);
        currentBalance += decA;
        printf("The main server received transactions from Server A using UDP over %d\n", PORT_serverA);
        printf("The main server sent a request to server B\n");
        int decB = query_balance(user, PORT_serverB, use_xor);
        currentBalance += decB;
        printf("The main server received transactions from Server B using UDP over %d\n", PORT_serverB);
        printf("The main server sent a request to server C\n");
        int decC = query_balance(user, PORT_serverC, use_xor);
        currentBalance += decC;
        printf("The main server received transactions from Server C using UDP over %d\n", PORT_serverC);

        if (currentBalance == 1000)
        {
            char reply[MAXLINE];
            snprintf(reply, sizeof(reply), "ERROR_USER %s", user);
            send(clientSock, reply, strlen(reply), 0);
        }
        else
        {
            char reply[MAXLINE];
            snprintf(reply, sizeof(reply), "BALANCE %d", currentBalance);
            send(clientSock, reply, strlen(reply), 0);
            printf("The main server sent the current balance to the client.\n");
        }
    }
    else if ((tok.size() == 4 && tok[0] == "TX") || (tok.size() == 5 && tok[0] == "TX" && tok[4] == "XOR"))
    {
        const std::string sender = tok[1].c_str();
        bool use_xor = (tok.size() == 5);
        const std::string receiver = tok[2].c_str();
        const std::string amount = tok[3].c_str();
        printf("The main server received from %s to transfer %s coins to %s using TCP over %d.\n",
               sender.c_str(), amount.c_str(), receiver.c_str(), PORT_CLIENT_TCP);
        printf("The main server sent a request to server A\n");
        int sA = query_balance(sender.c_str(), PORT_serverA, use_xor);
        printf("The main server received the feedback from server A using UDP over port %d\n", PORT_serverA);
        printf("The main server sent a request to server B\n");
        int sB = query_balance(sender.c_str(), PORT_serverB, use_xor);
        printf("The main server received the feedback from server B using UDP over port %d\n", PORT_serverB);
        printf("The main server sent a request to server C\n");
        int sC = query_balance(sender.c_str(), PORT_serverC, use_xor);
        printf("The main server received the feedback from server C using UDP over port %d\n", PORT_serverC);
        int sender_Bal = 1000 + sA + sB + sC;
        printf("The main server sent a request to server A\n");
        int rA = query_balance(receiver.c_str(), PORT_serverA, use_xor);
        printf("The main server received the feedback from server A using UDP over port %d\n", PORT_serverA);
        printf("The main server sent a request to server B\n");
        int rB = query_balance(receiver.c_str(), PORT_serverB, use_xor);
        printf("The main server received the feedback from server B using UDP over port %d\n", PORT_serverB);
        printf("The main server sent a request to server C\n");
        int rC = query_balance(receiver.c_str(), PORT_serverC, use_xor);
        printf("The main server received the feedback from server C using UDP over port %d\n", PORT_serverC);
        int receiver_Bal = 1000 + rA + rB + rC;
        char reply[MAXLINE];
        if (sender_Bal == 1000 && receiver_Bal == 1000)
        {
            snprintf(reply, sizeof(reply), "ERROR_USERS %s %s", sender.c_str(), receiver.c_str());
        }
        else if (sender_Bal == 1000)
        {
            snprintf(reply, sizeof(reply), "ERROR_USER %s", sender.c_str());
        }
        else if (receiver_Bal == 1000)
        {
            snprintf(reply, sizeof(reply), "ERROR_USER %s", receiver.c_str());
        }
        else if (sender_Bal < std::stoi(amount))
        {
            snprintf(reply, sizeof(reply), "INSUFFICIENT %d", sender_Bal);
        }
        else
        {
            int a = query_maxid(PORT_serverA);
            int b = query_maxid(PORT_serverB);
            int c = query_maxid(PORT_serverC);
            int nextSerial = std::max(std::max(a, b), c) + 1;

            char chosen = commit_tx(nextSerial, sender, receiver, amount,
                                    use_xor, /*tried_init=*/{});
            if (!chosen)
            {
                snprintf(reply, sizeof(reply),
                         "INSUFFICIENT %d", sender_Bal);
                fprintf(stderr, "TX %d: all backends unresponsive\n", nextSerial);
            }
            else
            {
                int sender_balance = 1000 + query_balance(sender.c_str(), PORT_serverA, use_xor) + query_balance(sender.c_str(), PORT_serverB, use_xor) + query_balance(sender.c_str(), PORT_serverC, use_xor);
                snprintf(reply, sizeof(reply), "SUCCESS %d", sender_balance);
            }
        }
        send(clientSock, reply, strlen(reply), 0);
        printf("The main server sent the result of the transaction to the client.\n");
    }
    else
    {
        const char *reply = "ERROR";
        send(clientSock, reply, strlen(reply), 0);
    }
}

void handle_monitor_request(int MonitorSock)
{
    char buffer[1024];
    int n = recv(MonitorSock, buffer, sizeof(buffer), 0);
    if (n <= 0)
        return;
    buffer[n] = '\0';
    std::istringstream iss(buffer);
    std::vector<std::string> tok;
    std::string s;
    while (iss >> s)
        tok.push_back(s);
    bool use_xor = (tok.size() == 2);
    if (std::string(buffer) == "TXLIST" || std::string(buffer) == "TXLIST XOR")
    {
        printf("The main server received a sorted list request from the monitor using TCP over port 26218.\n");
        auto A = query_txlist(PORT_serverA, use_xor);
        auto B = query_txlist(PORT_serverB, use_xor);
        auto C = query_txlist(PORT_serverC, use_xor);
        std::vector<Tx> merged;
        merged.reserve(A.size() + B.size() + C.size());
        merged.insert(merged.end(), A.begin(), A.end());
        merged.insert(merged.end(), B.begin(), B.end());
        merged.insert(merged.end(), C.begin(), C.end());
        for (auto &t : merged)
        {
            t.from = decrypt_generic(t.from, use_xor);
            t.to = decrypt_generic(t.to, use_xor);
            t.amt = std::stoi(decrypt_generic(std::to_string(t.amt), use_xor));
        }
        std::sort(merged.begin(), merged.end(), [](auto &a, auto &b)
                  { return a.id < b.id; });

        std::ofstream fout("txchain.txt", std::ios::trunc);
        for (auto &t : merged)
        {
            fout << t.id << ' '
                 << t.from << ' '
                 << t.to << ' '
                 << t.amt << '\n';
        }
        fout.close();
        const char *res = "TXLIST_OK";
        send(MonitorSock, res, strlen(res), 0);
    }
    else
    {
        const char *usage = "USAGE: ./monitor TXLIST";
        send(MonitorSock, usage, strlen(usage), 0);
        printf("Unexpected monitor command: %s. %s\n", buffer, usage);
    }
}

int main()
{
    int clientListen = listener(PORT_CLIENT_TCP);
    int monitorListen = listener(PORT_MONITOR_TCP);
    udpSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpSock < 0)
    {
        perror("UDP socket");
        exit(1);
    }

    // Bounded recv so a partitioned backend cannot stall a coordinator thread.
    struct timeval tv{};
    tv.tv_sec = UDP_TIMEOUT_MS / 1000;
    tv.tv_usec = (UDP_TIMEOUT_MS % 1000) * 1000;
    setsockopt(udpSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in udp_addr{};
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_addr.s_addr = INADDR_ANY;
    udp_addr.sin_port = htons(PORT_serverM);
    if (bind(udpSock, (sockaddr *)&udp_addr, sizeof(udp_addr)) < 0)
    {
        perror("bind to UDP port 24218 failed");
        exit(1);
    }

    printf("The main server is up and running.\n");
    while (true)
    {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(clientListen, &set);
        FD_SET(monitorListen, &set);
        int maxfd = std::max(clientListen, monitorListen);
        int ready = select(maxfd + 1, &set, nullptr, nullptr, nullptr);
        if (ready < 0)
        {
            perror("select");
            continue;
        }

        if (FD_ISSET(clientListen, &set))
        {
            int clientSock = accept(clientListen, nullptr, nullptr);
            if (clientSock < 0)
            {
                perror("client");
                continue;
            }
            // Detached worker: many clients can be in-flight at once; UDP
            // coordination is serialized inside via udpMtx.
            std::thread([clientSock]
                        {
                handle_client_request(clientSock);
                close(clientSock); })
                .detach();
        }

        if (FD_ISSET(monitorListen, &set))
        {
            int monSock = accept(monitorListen, nullptr, nullptr);
            if (monSock < 0)
            {
                perror("monitor");
                continue;
            }
            std::thread([monSock]
                        {
                handle_monitor_request(monSock);
                close(monSock); })
                .detach();
        }
    }
    close(udpSock);
}
