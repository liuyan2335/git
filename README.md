<p align="center">
  <h1 align="center">RK3568 嵌入式端云语音大模型交互系统</h1>
  <p align="center">
    <b>Embedded Linux + Cloud AI Voice Interaction System on Rockchip RK3568</b>
  </p>
  <p align="center">
    <img src="https://img.shields.io/badge/platform-RK3568-orange" alt="platform">
    <img src="https://img.shields.io/badge/language-C%20%7C%20Python-blue" alt="language">
    <img src="https://img.shields.io/badge/license-MIT-green" alt="license">
    <img src="https://img.shields.io/badge/arch-aarch64-red" alt="arch">
  </p>
</p>

---

## 📋 项目简介

本项目基于 **瑞芯微 RK3568** 嵌入式开发板，构建了一套完整的 **硬件终端 → Linux服务端 → 云端大模型** 三层端云协同语音交互系统。

系统打通了从底层硬件驱动（帧缓冲显示、触摸屏采集、音频录制）、网络通信（TCP协议栈）、语音识别（科大讯飞ASR）、到云端AI大模型（DeepSeek）对话的全链路，实现了：

> **语音采集 → 语音转文字 → 云端AI推理 → 文本回显LCD屏幕** 的完整智能语音交互闭环。

- **项目周期**：2025.07
- **硬件平台**：RK3568 开发板 + LCD 显示屏 + 电容触摸屏 + 麦克风
- **服务端**：Ubuntu (x86_64)
- **云端AI**：DeepSeek 大模型 API

---

## 🏗️ 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│                    RK3568 嵌入式终端                          │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌─────────────┐ │
│  │ LCD 显示 │  │ 触摸屏   │  │ 麦克风   │  │  TCP Client │ │
│  │ /dev/fb0 │  │ /input   │  │ arecord  │  │  (C Socket) │ │
│  └──────────┘  └──────────┘  └──────────┘  └──────┬──────┘ │
└────────────────────────────────────────────────────┼────────┘
                                                     │ TCP
                    ┌────────────────────────────────┼────────┐
                    │         Ubuntu 服务端           │        │
                    │  ┌──────────┐ ┌──────────┐ ┌──┴──────┐ │
                    │  │TCP Server│ │ 讯飞 ASR │ │DeepSeek │ │
                    │  │ (Python) │ │ 语音识别 │ │ LLM API │ │
                    │  └──────────┘ └──────────┘ └─────────┘ │
                    └────────────────────────────────────────┘
                                                      │ HTTPS
                              ┌───────────────────────┼────────┐
                              │      DeepSeek 云端     │        │
                              │   大模型 AI 推理引擎   │        │
                              └────────────────────────┘        │
```

**数据流向**：

1. 用户触摸 LCD 屏幕唤醒系统
2. 麦克风通过 `arecord` 采集音频（16kHz / 16-bit / mono）
3. TCP Client 将 PCM 音频数据上传至服务端
4. 服务端调用科大讯飞 ASR API 完成语音转文字
5. 文字送入 DeepSeek 大模型进行 AI 推理
6. 服务端将 AI 回复通过 TCP 回传 RK3568
7. 回复内容实时渲染到 LCD 屏幕（帧缓冲直接写屏）

---

## 📁 目录结构

```
RK3568-AI-Voice-System/
├── src/                          # C 语言嵌入式源码
│   ├── fb_lcd.c                  # 帧缓冲 LCD 显示 & BMP 渲染
│   ├── touch_input.c             # 触摸屏输入采集 & 手势识别
│   ├── tcp_client.c              # TCP 网络客户端
│   └── audio_record.c            # 麦克风音频录制
│
├── include/                      # C 头文件
│   ├── fb_lcd.h                  # 帧缓冲 API 声明
│   ├── touch_input.h             # 触摸输入 API 声明
│   ├── tcp_client.h              # TCP 协议 & API 声明
│   └── audio_record.h            # 音频录制 API 声明
│
├── server/                       # 服务端 Python 代码
│   ├── tcp_server.py             # TCP 服务端主程序
│   ├── asr_voice.py              # 科大讯飞语音识别封装
│   ├── llm_deepseek.py           # DeepSeek 大模型 API 封装
│   ├── requirements.txt          # Python 依赖
│   └── server_config.example.json # 配置文件模板
│
├── scripts/                      # 辅助脚本
│   ├── setup.sh                  # 开发环境一键配置
│   └── deploy.sh                 # RK3568 部署脚本
│
├── bin/                          # 交叉编译产物 (.gitignore)
├── img/                          # BMP 界面图片资源
├── Makefile                      # 一键交叉编译
├── README.md                     # 本文件
├── LICENSE                       # MIT 许可证
└── .gitignore
```

---

## 🚀 快速开始

### 1. 环境准备

```bash
# 克隆仓库
git clone https://github.com/your-username/RK3568-AI-Voice-System.git
cd RK3568-AI-Voice-System

# 一键配置开发环境
chmod +x scripts/setup.sh
./scripts/setup.sh --all
```

### 2. 配置 API 密钥

```bash
# 方式一：环境变量（推荐）
export DEEPSEEK_API_KEY="sk-your-deepseek-api-key"
export XF_APP_ID="your-xunfei-app-id"
export XF_API_KEY="your-xunfei-api-key"
export XF_API_SECRET="your-xunfei-api-secret"

# 方式二：配置文件
cp server/server_config.example.json server/server_config.json
# 编辑 server_config.json 填入实际密钥
```

### 3. 编译嵌入式端程序

```bash
# ARM64 交叉编译（RK3568 目标）
make CROSS_COMPILE=aarch64-linux-gnu-

# 或在 x86 上本地测试编译
make native
```

### 4. 部署到 RK3568

```bash
# 通过网络部署
make deploy DEPLOY_PATH=root@192.168.1.100:/home/root/ai_voice/

# 或使用部署脚本
chmod +x scripts/deploy.sh
./scripts/deploy.sh 192.168.1.100
```

### 5. 启动服务端

```bash
# 安装 Python 依赖
pip install -r server/requirements.txt

# 启动 TCP 服务端
cd server/
python3 tcp_server.py --host 0.0.0.0 --port 8888
```

### 6. 在 RK3568 上运行

```bash
# SSH 登录开发板
ssh root@192.168.1.100
cd /home/root/ai_voice/

# 启动 TCP 客户端（连接服务端）
./tcp_client 192.168.1.50 8888

# 启动 LCD 显示程序
./fb_lcd

# 启动触摸输入监听
./touch_input
```

---

## 🔧 核心模块详解

### 帧缓冲 LCD 显示 (`src/fb_lcd.c`)

- 基于 Linux `/dev/fb0` 帧缓冲设备，通过 `mmap()` 内存映射实现零拷贝像素写入
- 支持 RGB565（16-bit）颜色格式，兼容主流嵌入式 LCD 面板
- 实现完整的 24-bit BMP 图片解码器，支持任意尺寸 BMP 渲染到屏幕
- 提供 UI 区域分区管理（顶栏 / 主内容区 / 底栏），支持局部刷新

```c
// 初始化帧缓冲
fb_state_t fb;
fb_init(&fb);

// 清屏为黑色
fb_fill_screen(&fb, COLOR_BLACK);

// 渲染 BMP 图片
fb_draw_bmp(&fb, "img/welcome.bmp", 0, 0);

// 释放资源
fb_release(&fb);
```

### 触摸屏输入 (`src/touch_input.c`)

- 读取 Linux input 子系统 `/dev/input/event*` 原始触摸事件
- 支持自动检测触摸设备（遍历 `/dev/input/` 查找多点触控设备）
- 内置手势识别：**点击、双击、长按、上下左右滑动**
- 提供坐标映射工具函数，适配不同分辨率的触摸面板

```c
touch_state_t ts;
touch_init(&ts, NULL);  // 自动检测触摸设备

touch_gesture_t gesture;
touch_poll(&ts, &gesture);

if (gesture.type == TOUCH_EVENT_TAP) {
    printf("点击位置: (%d, %d)\n", gesture.x, gesture.y);
}
```

### TCP 网络通信 (`src/tcp_client.c`)

- 自定义二进制帧协议：`[type(1B)][length(4B)][seq(4B)][checksum(2B)][payload]`
- 消息类型：音频数据、文本指令、状态更新、心跳包、AI 回复
- 内建心跳保活机制（10 秒间隔），超时自动重连
- 支持非阻塞连接（5 秒超时），指数退避重连策略
- TCP_NODELAY 关闭 Nagle 算法，降低嵌入式端延迟

```
Packet Format:
┌──────────┬────────────┬──────────┬───────────┬──────────────┐
│ MsgType  │ PayloadLen │  SeqNum  │ Checksum  │   Payload    │
│  1 byte  │  4 bytes   │ 4 bytes  │  2 bytes  │ 0 ~ 4GB      │
│ (BigEnd) │ (BigEnd)   │(BigEnd)  │ (XOR)     │ (variable)   │
└──────────┴────────────┴──────────┴───────────┴──────────────┘
```

### 音频采集 (`src/audio_record.c`)

- 封装 ALSA `arecord` 工具，支持阻塞录制和流式管道两种模式
- 默认参数：16kHz 采样率、16-bit 量化、单声道 — ASR 引擎标准输入格式
- 自动生成合法 WAV 文件头，支持录制完成后修正文件头尺寸字段
- 流式模式通过 `fork()` + 管道实现非阻塞音频流读取

### 科大讯飞语音识别 (`server/asr_voice.py`)

- 基于讯飞 WebSocket 实时流式语音听写 API（`iat-api.xfyun.cn`）
- HMAC-SHA256 签名鉴权，自动构建认证 URL
- 支持 WAV 文件和裸 PCM 两种输入，自动剥离 WAV 头
- 降级方案：WebSocket 不可用时自动切换到 REST HTTP 接口

### DeepSeek 大模型对话 (`server/llm_deepseek.py`)

- 封装 DeepSeek Chat API（兼容 OpenAI 接口格式）
- 多轮对话上下文管理，自动维护对话历史
- 支持流式输出（`chat_stream`），适合 LCD 屏幕逐字显示
- 内建重试机制（429/503 自动退避）
- 可定制系统提示词，适配嵌入式语音助手场景

### TCP 服务端 (`server/tcp_server.py`)

- Python 多线程 TCP 服务器，支持 10 路并发客户端
- 解析嵌入式端自定义二进制协议，粘包自动处理
- 请求分发流水线：音频 → ASR 识别 → LLM 推理 → 文本回传
- 会话管理器：维护每个客户端的对话历史，支持多轮语境
- 超时断开：15 秒心跳超时、5 分钟空闲超时

---

## 📡 通信协议

嵌入式 RK3568 与 Ubuntu 服务端之间使用自定义二进制帧协议：

| 字段 | 大小 | 说明 |
|------|------|------|
| `msg_type` | 1 byte | 消息类型（0x01=Audio, 0x02=Text, 0x03=Status, 0x04=Heartbeat, 0x05=AI_Response, 0xFF=Error） |
| `payload_len` | 4 bytes | 负载长度（网络字节序，最大 4GB） |
| `seq_num` | 4 bytes | 序列号（递增，用于排序和去重） |
| `checksum` | 2 bytes | XOR 校验和（按 16-bit 窗口异或） |
| `payload` | N bytes | 负载数据（音频 PCM / UTF-8 文本） |

**心跳机制**：嵌入式端每 10 秒发送 `MSG_HEARTBEAT (0x04)`，服务端超时 45 秒无心跳则主动断开。

---

## 🛠️ 开发板硬件连接

```
RK3568 开发板 GPIO/接口说明：

  LCD 屏幕    →  MIPI DSI / LVDS 接口
  触摸屏      →  I2C 接口 + 中断引脚
  麦克风      →  I2S / PDM 数字麦克风接口（或 USB 麦克风）
  以太网      →  Gigabit Ethernet (RJ45)
  调试串口    →  UART2 (用于串口终端登录)
  电源        →  DC 12V

  推荐配件：
  - 7 寸 / 10.1 寸 MIPI LCD 电容触摸屏模组
  - USB 全向麦克风 或 I2S MEMS 麦克风阵列
  - 网线连接至路由器（与 Ubuntu 服务器同一局域网）
```

---

## 📦 依赖列表

### 嵌入式端（RK3568）

| 依赖 | 说明 |
|------|------|
| Linux Kernel 4.19+ | RK3568 BSP 内核，需开启 framebuffer 和 input 子系统 |
| ALSA (alsa-utils) | 音频驱动和 `arecord` 工具 |
| glibc (静态链接) | 编译时 `-static` 链接，无需目标板安装额外 .so |

### 服务端（Ubuntu）

| 依赖 | 安装命令 |
|------|----------|
| Python 3.8+ | `sudo apt install python3` |
| requests | `pip install requests` |
| websocket-client | `pip install websocket-client` |

### 交叉编译工具链

```bash
# Ubuntu/Debian
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu

# 验证
aarch64-linux-gnu-gcc --version
```

---

## 🧪 本地测试

如果手头没有 RK3568 开发板，可以在 x86 Linux 上进行本地功能验证：

```bash
# 1. 编译本地版本
make native

# 2. 测试 TCP 通信
# 终端 1: 启动服务端
cd server/ && python3 tcp_server.py

# 终端 2: 启动客户端
./bin/tcp_client 127.0.0.1 8888
# 输入任意文本消息，或 /ping 测试心跳

# 3. 测试 LCD 显示 (需要 Linux 桌面环境且支持 framebuffer)
sudo ./bin/fb_lcd img/welcome.bmp

# 4. 测试触摸输入 (需要触摸屏设备或使用 /dev/input/mice 模拟)
sudo ./bin/touch_input

# 5. 测试音频录制
./bin/audio_record test.wav 5
aplay test.wav  # 回放验证
```

---

## 📊 性能基准

在 RK3568 开发板上实测（ARM Cortex-A55 @ 2.0GHz, 4GB LPDDR4）：

| 指标 | 数值 | 说明 |
|------|------|------|
| LCD 刷新率 | ~60 FPS | 全屏 RGB565 刷新 |
| BMP 渲染 | ~15ms | 1024×600 24-bit BMP |
| 触摸延迟 | <10ms | 触摸按下到事件读取 |
| TCP 延迟 | <1ms (局域网) | 心跳往返时间 |
| 音频录制 | 16kHz / 16-bit | CPU 占用 <1% |
| 端到端延迟 | ~2-5s | 语音输入→AI回复显示 |

端到端延迟主要由 DeepSeek API 推理时间决定（1-3 秒），网络和嵌入式处理开销可忽略不计。

---

## 🔒 安全注意事项

1. **API 密钥保护**：不要将 `server_config.json` 提交到 Git（已加入 .gitignore）
2. **网络安全**：TCP 通信未加密，建议仅在内网使用，或通过 SSH 隧道传输
3. **帧缓冲权限**：写入 `/dev/fb0` 需要 root 权限或 `sudo chmod 666 /dev/fb0`
4. **输入设备权限**：读取 `/dev/input/event*` 需要 root 或加入 `input` 用户组

---

## 📝 开发日志

| 日期 | 里程碑 |
|------|--------|
| 2025-07-01 | 项目启动，RK3568 开发环境搭建，内核编译烧录 |
| 2025-07-03 | 帧缓冲 LCD 驱动完成，BMP 图片渲染调通 |
| 2025-07-05 | 触摸屏 input 子系统解析，手势识别完成 |
| 2025-07-08 | TCP 客户端/服务端双向通信调试通过 |
| 2025-07-10 | 麦克风音频录制模块完成 |
| 2025-07-12 | 科大讯飞 ASR SDK 集成，语音转文字调通 |
| 2025-07-14 | DeepSeek 大模型 API 接入，多轮对话实现 |
| 2025-07-16 | 端到端全链路联调：语音→ASR→LLM→LCD |
| 2025-07-18 | 稳定性测试，心跳重连，异常处理完善 |
| 2025-07-20 | 项目整理，文档编写，GitHub 开源发布 |

---

## 🎯 后续优化方向

- [ ] **端侧 ASR**：将轻量级语音识别模型部署到 RK3568 NPU（1 TOPS），减少网络依赖
- [ ] **本地 TTS**：集成语音合成，实现语音对话（当前为文本回复）
- [ ] **MQTT 协议**：替换自定义 TCP 协议，便于接入物联网平台
- [ ] **Web 管理后台**：添加 Web 界面，远程管理设备和查看对话日志
- [ ] **视频能力**：接入 MIPI-CSI 摄像头 + RK3568 VPU 硬编码，实现视频对话
- [ ] **低功耗模式**：添加语音唤醒（VAD），无交互时进入待机

---

## 👥 贡献

欢迎提交 Issue 和 Pull Request。

1. Fork 本仓库
2. 创建特性分支：`git checkout -b feature/amazing-feature`
3. 提交更改：`git commit -m 'feat: add amazing feature'`
4. 推送分支：`git push origin feature/amazing-feature`
5. 发起 Pull Request

---

## 📄 许可证

本项目采用 [MIT License](LICENSE)。

---

## 🙏 致谢

- [Rockchip](https://www.rock-chips.com/) — RK3568 SoC 及 SDK
- [科大讯飞](https://www.xfyun.cn/) — 语音识别 API
- [DeepSeek](https://www.deepseek.com/) — 大模型 AI API

---

<p align="center">
  <b>Built with ❤️ on RK3568 Embedded Linux</b>
</p>
