# 🌸 Isolate - AI 智能交互文字冒险游戏

**Isolate** 是一款基于 C++17 和 Qt5 开发的次世代文字冒险游戏。我们打破了传统 Galgame 固定选项的桎梏，致力于将本地状态机与大语言模型（LLM）深度结合，为玩家提供真正自由的对话交互体验。



**核心理念**：我们拒绝按部就班的剧本，玩家的每一次语言沟通，都是解锁隐藏剧情与特殊结局的钥匙。

**[点击阅读：项目愿景与 AI 底层架构](docs/Vision_and_Design.md)**

---

## 🛠️ 技术栈 (Tech Stack)
- **核心语言**: C++17
- **图形界面**: Qt5 (高性能原生桌面 GUI)
- **构建系统**: CMake
- **数据持久化**: SQLite3 (支持多档位、多用户的读档/存档)
- **核心算法**: Trie 字典树 (毫秒级敏感词过滤)、正则表达式匹配
- **数据驱动**: `nlohmann/json` (剧情与代码彻底解耦)

## 🚀 快速开始 (Quick Start)

### 环境依赖
在 Ubuntu 环境下，请确保安装了 Qt5 和 SQLite3 开发库：
```bash
sudo apt install qtbase5-dev libsqlite3-dev