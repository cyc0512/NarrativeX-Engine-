// ---------------------------------------------------------------------------
//  NarrativeX Engine :: nx/Types.hpp
//  基础类型与无依赖工具。整个 nx/ 内核只依赖 C++17 标准库，
//  不依赖 Qt / libcurl / SQLite —— 这样内核可以被单元测试、服务端、
//  命令行工具直接复用，而这些外围依赖只出现在适配层。
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <stdexcept>

namespace nx {

// ---- 基础别名 -------------------------------------------------------------
using NodeId = std::string;
using TagId = std::string;
using ItemId = std::string;
using Seq = std::int64_t;          // time_cursor 的类型。用有符号以便检测异常
using Score = double;

// ---- 错误体系（ Phase 要求：类型化错误，绝不把内部细节抛给调用方 ）--------
class NxError : public std::runtime_error {
public:
    enum class Code {
        BadScript,       // 剧本结构/条件非法
        BadRoute,        // 目标节点不存在
        Deadlock,        // Frontier 为空且无兜底
        CursorRegress,   // time_cursor 试图回退（违反单调性）
        Concurrency,     // 乐观锁失败
        Budget,          // token 预算异常
    };
    NxError(Code c, const std::string& what) : std::runtime_error(what), code_(c) {}
    Code code() const noexcept { return code_; }
private:
    Code code_;
};

inline const char* to_string(NxError::Code c) noexcept {
    switch (c) {
        case NxError::Code::BadScript:     return "BadScript";
        case NxError::Code::BadRoute:      return "BadRoute";
        case NxError::Code::Deadlock:      return "Deadlock";
        case NxError::Code::CursorRegress: return "CursorRegress";
        case NxError::Code::Concurrency:   return "Concurrency";
        case NxError::Code::Budget:        return "Budget";
    }
    return "Unknown";
}

// ---- 确定性随机数生成器 (splitmix64) ---------------------------------------
//  【为什么不用 std::mt19937 / std::random_device】
//  种子必须随存档持久化。一旦使用了不可控熵源，同一份存档两次加载会走出
//  不同的 Frontier 抽样结果 —— 这正是用户担心的"读档时空错乱"的一种表现。
class Rng {
public:
    explicit Rng(std::uint64_t seed = 0x9E3779B97F4A7C15ull) noexcept : s_(seed) {}

    std::uint64_t next_u64() noexcept {
        s_ += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = s_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // [0, 1)
    double next_double() noexcept {
        return static_cast<double>(next_u64() >> 11) * (1.0 / (1ull << 53));
    }

    std::uint64_t state() const noexcept { return s_; }
    void setState(std::uint64_t s) noexcept { s_ = s; }

private:
    std::uint64_t s_;
};

// ---- 小工具 ---------------------------------------------------------------

inline bool contains(const std::vector<std::string>& v, const std::string& key) {
    for (const auto& x : v) if (x == key) return true;
    return false;
}

// UTF-8 安全的粗略 token 估算：CJK 每字 ~0.7 token，其余每 4 字节 ~1 token。
// 只用于预算裁剪，不需要精确 tokenizer，也避免引入 tiktoken 之类的额外依赖。
inline std::size_t estimate_tokens(const std::string& text) {
    std::size_t cjk = 0, ascii = 0;
    const std::size_t n = text.size();
    for (std::size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        if (c >= 0xF0)      len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        // CJK 基本区 U+4E00~U+9FFF 的 UTF-8 首字节落在 0xE4~0xE9
        if (len == 3 && c >= 0xE4 && c <= 0xE9) ++cjk;
        else                                    ++ascii;
        i += len;
    }
    return static_cast<std::size_t>(cjk * 0.7 + ascii / 4.0) + 1;
}

}  // namespace nx
