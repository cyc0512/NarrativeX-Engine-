#include <iostream>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

int main() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "连接失败！\n";
        return -1;
    }

    char buffer[2048] = {0};
    recv(sock, buffer, sizeof(buffer), 0);
    std::cout << buffer;

    while (true) {
        std::cout << "\033[34m> \033[0m"; 
        std::string input;
        std::getline(std::cin, input);
        input += "\n";
        send(sock, input.c_str(), input.length(), 0);

        memset(buffer, 0, sizeof(buffer));
        int bytesReceived = recv(sock, buffer, sizeof(buffer), 0);
        if (bytesReceived <= 0) break;
          // 绿色显示系统
        std::cout << "\033[32m" << buffer << "\033[0m"; 
    }
    close(sock);
    return 0;
}