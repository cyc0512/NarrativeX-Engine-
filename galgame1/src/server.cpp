#include <iostream>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>

// 注意：因为配置了 CMake 搜索路径，这里直接用文件名即可
#include "GameEngine.h"       
#include "SecurityFilter.h"   // 👈 【新增】引入安全过滤器

int main(int argc, char* argv[]) {
    // 1. 初始化引擎和安检
    std::string storyPath = "../data/story.json"; 
    if (argc > 1) storyPath = argv[1];
    GameEngine engine(storyPath);
    SecurityFilter security; 

    // 2. 网络基础建设
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(8080);
    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 3);
    
    std::cout << "【服务端】启动成功，等待连接...\n";

    // 👇 这里是新增的外层循环：允许接待多个客户端
    while (true) {
        sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        std::cout << "【服务端】正在等待下一个玩家连接...\n";
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        std::cout << "【服务端】玩家已连接！IP: " << inet_ntoa(client_addr.sin_addr) << "\n";

        // 发送欢迎语
        std::string welcome = "【系统】欢迎来到文字游戏，请先输入你的名字:\n[你]: ";
        send(client_fd, welcome.c_str(), welcome.length(), 0);

        char buffer[1024] = {0};
        // 内层循环：专门处理当前玩家的聊天
        while (true) {
            memset(buffer, 0, sizeof(buffer));
            int bytesReceived = recv(client_fd, buffer, sizeof(buffer), 0);
            
            //  如果 recv 返回 <= 0，说明玩家掉线或主动退出了！
            if (bytesReceived <= 0) {
                std::cout << "【服务端】玩家已下线，释放连接。\n\n";
                break; // 跳出内层聊天循环，回到外层 accept 循环
            }

            std::string userInput(buffer);
            if (!userInput.empty() && userInput.back() == '\n') userInput.pop_back();
            
            if (userInput == "退出" || userInput == "结束") {
                std::string bye = "【系统】再见！\n";
                send(client_fd, bye.c_str(), bye.length(), 0);
                break;
            }

            // 安检
            if (security.checkSecurity(userInput)) {
                std::string warning = "\n【系统警告】检测到不文明用语，小雅生气地跑开了！\n【游戏结束】";
                send(client_fd, warning.c_str(), warning.length(), 0);
                break; 
            }

            // 交给引擎处理
            std::string reply = engine.processInput(userInput);
            reply += "\n[你]: "; 
            send(client_fd, reply.c_str(), reply.length(), 0);
        }

        // 👇 关键：玩家断开后，必须关掉这个专属的 client_fd，否则会造成文件描述符泄漏！
        close(client_fd);
    }

    // 如果外层 while 被 break 了，才会执行到这里（实际上一般跑不到这里）
    close(server_fd);
    return 0;
}