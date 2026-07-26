# AuRunning

AuRunning 是面向 Windows x64 本地语音助手的原生全双工音频运行时。它统一持有默认通信麦克风和扬声器，通过 WebRTC Audio Processing Module（APM）执行 AEC3 回声消除、高通滤波、噪声抑制和语音活动检测（VAD），再向上层发布经过处理的 16 kHz 单声道 PCM。

AuRunning 只负责实时音频输入、输出与回声控制，不包含唤醒词、语音识别、语音合成或大语言模型能力。

## 主要能力

- 使用 Windows WASAPI 统一管理麦克风采集与扬声器播放。
- 将实际播放的 TTS 音频送入 WebRTC APM 反向流，为 AEC3 提供时间一致的远端参考信号。
- 对麦克风音频执行 AEC3、高通滤波和高等级噪声抑制。
- 将 48 kHz 设备音频重采样为 16 kHz、单声道、PCM16、10 ms 帧。
- 播放期间检测近端人声，发布 `near_end_start` 和 `near_end_end` 事件以支持语音打断。
- 使用有界采集队列和播放队列控制内存占用与网络背压。
- 监控默认通信设备变化、设备失活、WASAPI 连续错误以及采集/播放停滞。
- 通过仅监听回环地址的 WebSocket 接口与上层编排服务通信。

## 系统要求

- Windows 10/11 x64
- Visual Studio 2022，包含“使用 C++ 的桌面开发”工作负载
- uv，用于获取固定主版本的 CMake
- CMake 3.24-3.x（暂不支持 CMake 4.x）
- 支持 AVX2 的 x64 处理器
- 可访问 CMake `FetchContent` 中固定版本的第三方依赖

当前版本仅支持以下音频参数组合：

- 设备采样率：48 kHz
- 采集输出采样率：16 kHz
- 帧长：10 ms

## 构建

在 Visual Studio 2022 x64 Developer PowerShell 中执行：

```powershell
uvx --from "cmake<4" cmake -S . -B build -A x64
uvx --from "cmake<4" cmake --build build --config Release --target au-running --parallel 1
```

固定版本的 WebRTC APM 依赖在受支持的构建环境中并行编译不够稳定，因此建议保留 `--parallel 1`。

生成的可执行文件位于：

```text
build/Release/au-running.exe
```

## 运行

独立运行示例：

```powershell
.\build\Release\au-running.exe --config .\configs\config.json
```

由 ChatCaht 托管时，ChatCaht 会根据自身 `audio:` 配置生成运行时配置，并通过 `--config` 传入：

```powershell
.\build\Release\au-running.exe --config ..\ChatCaht\artifacts\run\au-running.generated.json
```

`configs/config.json` 只用于独立开发。托管部署应由编排服务生成唯一的生效配置，避免维护两份生产参数。

## 数据链路

```text
麦克风
  -> WASAPI 采集
  -> AEC3 / 高通 / 噪声抑制 / VAD
  -> 16 kHz PCM
  -> WebSocket
  -> WakeUp / SpText

TTS PCM
  -> WebSocket
  -> 有界播放队列
  -> WASAPI 扬声器
  -> WebRTC APM 反向参考流
```

## WebSocket 接口

默认地址：

```text
ws://127.0.0.1:8810/v1/audio/ws
```

服务仅面向本机回环地址。文本消息用于控制和事件，服务端发送的二进制消息为经过 AEC 处理的 16 kHz、16 位、单声道 PCM。

连接建立后，服务首先发送 `status`。上层只有在 `ready: true` 和 `aec_ready: true` 时才能启动语音会话。当前协议版本为 `2`。

### 控制消息

- `status`：获取状态、设备信息、生效配置和 AEC 指标。
- `ping`：连接探活。
- `shutdown`：请求服务退出。
- `play_start`：创建一个逻辑播放流。
- `play_chunk`：声明并发送一块二进制 PCM。
- `play_end`：结束播放流，并等待播放队列排空。
- `play_cancel`：立即清空当前播放队列。

### 服务事件

- `near_end_start`：播放期间确认检测到近端人声。
- `near_end_end`：近端人声结束。
- `playback_started`：播放流开始。
- `playback_ended`：播放流结束或被取消。

播放队列默认上限为 500 ms，并对生产端施加背压。取消或替换播放时会推进播放代际令牌，防止已阻塞的旧音频块在取消后重新进入队列。采集传输队列默认上限为 1 秒；持续背压超过阈值后，服务会将整体健康状态标记为不可用。

## 配置

默认开发配置位于 `configs/config.json`：

| 参数 | 默认值 | 说明 |
| --- | ---: | --- |
| `host` | `127.0.0.1` | WebSocket 监听地址 |
| `port` | `8810` | WebSocket 端口 |
| `ws_path` | `/v1/audio/ws` | WebSocket 路径 |
| `device_sample_rate` | `48000` | WASAPI 设备采样率 |
| `capture_sample_rate` | `16000` | 对上层发布的采样率 |
| `frame_ms` | `10` | 音频帧长度 |
| `render_queue_ms` | `500` | 播放队列容量 |
| `capture_queue_ms` | `1000` | 采集传输队列容量 |
| `capture_stall_timeout_ms` | `3000` | 采集或播放停滞超时 |
| `aec_tail_ms` | `200` | 播放结束后的 AEC 尾部保护时间 |
| `barge_in_min_speech_ms` | `120` | 确认近端人声所需的连续时长 |
| `barge_in_hangover_ms` | `300` | 近端人声结束判定延迟 |
| `vad_aggressiveness` | `2` | WebRTC VAD 激进程度，范围为 0-3 |
| `input_device` | `null` | 输入设备；为空时使用默认通信设备 |
| `output_device` | `null` | 输出设备；为空时使用默认通信设备 |

## 健康检查与恢复

AuRunning 运行期间会检查：

- 选定的输入和输出设备是否仍处于活动状态。
- 未绑定明确设备时，Windows 默认通信设备是否发生变化。
- 麦克风采集帧是否持续到达。
- 播放队列非空时，渲染进度是否持续推进。
- WASAPI 是否出现设备失效或连续调用错误。
- WebSocket 采集传输是否发生持续背压。

设备失效、连续五次 WASAPI 错误、默认通信设备变化或采集/播放停滞超过阈值时，服务会标记为不可用并以退出码 `4` 退出。进程管理器应重新启动 AuRunning，以完整重建 WASAPI、APM、VAD、重采样器和传输线程。

初始化失败时退出码为 `3`，WebSocket 监听失败时退出码为 `2`。

## 第三方依赖

依赖版本固定在 `CMakeLists.txt` 中：

- IXWebSocket
- nlohmann/json
- Abseil
- WebRTC Audio Processing Module

构建时由 CMake `FetchContent` 下载。发布二进制文件前，应根据实际分发方式核对各依赖许可证并随发布包附带所需声明。
