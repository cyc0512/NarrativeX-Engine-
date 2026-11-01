// R-02 端到端验证：中文 / emoji / 中文引号「」/ 全角标点是否完整可见。
//
// 为什么必须端到端：旧的清洗逻辑 `if (c >= 32 && c <= 126) clean_str += c;`
// 会把所有 >= 0x80 的 UTF-8 字节静默删光，但删光之后 JSON **照样能解析成功**
// （因为被删的是字符串内容，不是结构），所以任何"JSON parse 没报错"式的单测
// 都测不出这个 bug，只有真的把内容打出来看才知道。
//
// 本程序走的是真实生产链路：
//   本地 mock HTTP 服务 -> libcurl -> AiEngine::analyzeInput
//   -> sanitizeJsonText -> nlohmann::json::parse -> GameEngine::processInput -> reply
//
// 用法（必须在 build/ 目录下运行，AiEngine 读的是 ../config.json）：
//   cd build && ./e2e_chinese
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "GameEngine.h"

namespace {

// 期望在最终 reply 里出现的全部片段。
// 这四类正好覆盖"只保留 ASCII"会被杀掉的每一种字节形态：
//   中文汉字      -> 3 字节 UTF-8 (0xE4 0xBD 0xA0 ...)
//   「」中文引号   -> 3 字节 UTF-8
//   🌟🌙 emoji     -> 4 字节 UTF-8 (0xF0 0x9F ...)
//   。…… 全角标点  -> 3 字节 UTF-8
struct Expectation {
    const char* name;
    const char* needle;
    const char* why;
};

const Expectation kExpectations[] = {
    {"中文汉字",   "【中文测试】", "3 字节 UTF-8，ASCII 过滤后会被整段吃掉"},
    {"中文引号",   "「发卡」",     "3 字节 UTF-8，且是 U+300C/U+300D 非 ASCII 标点"},
    {"emoji",      "\xF0\x9F\x8C\x9F", "4 字节 UTF-8（U+1F31F 🌟）"},
    {"emoji-2",    "\xF0\x9F\x8C\x99", "4 字节 UTF-8（U+1F319 🌙）"},
    {"全角句号",   "。",           "3 字节 UTF-8（U+3002）"},
    {"全角省略号", "……",           "3 字节 UTF-8（U+2026 x2）"},
};

// 打印一段 UTF-8 里所有非 ASCII 字节，肉眼可确认"字节没被吞"。
void DumpNonAsciiBytes(const std::string& s) {
    std::string hex;
    int count = 0;
    for (unsigned char c : s) {
        if (c >= 0x80) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%02X ", c);
            hex += buf;
            ++count;
        }
    }
    std::printf("    非 ASCII 字节数 = %d\n", count);
    std::printf("    字节序列(前 60 个) = %s\n", hex.substr(0, 180).c_str());
}

// 记录一次"不检查内容、只检查有没有崩"的观察型用例。
// 用于在报告里如实说明：非法 UTF-8 输入会得到「（系统异常）」而不是崩溃。
int RunObserveOnly(GameEngine& engine, const char* label, const std::string& freeInput) {
    std::printf("\n--- 观察用例: %s ---\n", label);
    engine.createNewPlayer("e2e_cn");
    engine.processInput("1");
    std::string reply = engine.processInput(freeInput);
    std::printf("    AI 回复 : %s\n", reply.c_str());
    std::printf("    [观察] 进程未崩溃 = OK（异常被 AiEngine 的 catch 兜住）\n");
    (void)reply;
    return 0;
}

int RunCase(GameEngine& engine, const char* label, const std::string& freeInput) {
    std::printf("\n--- 用例: %s ---\n", label);

    // 回到节点 0 再走选项 1，确保落在带 intent_nodes 的自由输入节点（节点 1）。
    engine.createNewPlayer("e2e_cn");
    std::string r1 = engine.processInput("1");
    std::printf("    进入关键节点: currentNode=%s\n", engine.getCurrentNode().c_str());
    (void)r1;

    std::string reply = engine.processInput(freeInput);
    std::printf("    玩家输入: %s\n", freeInput.c_str());
    std::printf("    AI 回复 : %s\n", reply.c_str());
    DumpNonAsciiBytes(reply);

    int miss = 0;
    for (const Expectation& e : kExpectations) {
        bool hit = reply.find(e.needle) != std::string::npos;
        std::printf("    [%s] %-10s %s (%s)\n",
                    hit ? "OK" : "NG", e.name,
                    hit ? "完整可见" : "缺失", e.why);
        if (!hit) ++miss;
    }
    if (reply.empty()) {
        std::printf("    [NG] 回复为空 —— 这正是 R-02 的症状（内容被静默清空）\n");
        ++miss;
    }
    return miss;
}

}  // namespace

int main() {
    std::printf("=== R-02 端到端中文验证（真实 HTTP + 真实 AiEngine + 真实 GameEngine）===\n");

    GameEngine engine("../data/story.json");
    if (!engine.ready()) {
        std::printf("FATAL 引擎构造失败: %s\n", engine.lastError().c_str());
        return 2;
    }

    int totalMiss = 0;
    totalMiss += RunCase(engine, "纯中文自由输入", "我想看看你口袋里的是什么");
    totalMiss += RunCase(engine, "中文 + emoji 自由输入", "嘿嘿 😏 让我看看「那个东西」");

    // 200 字合法 UTF-8 中文（"哈" = E5 93 88），注意不能用 '哈' 这种
    // multi-character literal —— 那是 int 常量，写进 std::string 只会拿到一个字节垃圾。
    std::string longCn;
    for (int i = 0; i < 200; ++i) longCn += "\xE5\x93\x88";
    longCn += "【发卡】";
    totalMiss += RunCase(engine, "超长中文输入(200 字)", longCn);

    // 观察用例：非法 UTF-8（裸 0x80 字节）会触发 nlohmann json::dump() 的
    // type_error.316，被 AiEngine 的 catch(const std::exception&) 兜住，
    // 表现为「（系统异常）」。不崩溃，但值得单独立案。
    std::string badUtf8 = "\x80\x81\x82\xFF";
    RunObserveOnly(engine, "非法 UTF-8 输入(仅观察是否崩溃)", badUtf8);

    std::printf("\n=========================================\n");
    if (totalMiss == 0) {
        std::printf("结果: R-02 通过 —— 中文 / emoji / 「」/ 全角标点全部 100%% 完整可见\n");
    } else {
        std::printf("结果: R-02 失败 —— 共 %d 项缺失\n", totalMiss);
    }
    std::printf("=========================================\n");
    return totalMiss == 0 ? 0 : 1;
}
