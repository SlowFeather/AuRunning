# AuRunning 配置说明

`config.json` 是 AuRunning 的运行时配置。JSON 标准不支持注释，因此把字段用途集中写在这里，修改后由 ChatCaht 生成或直接启动 AuRunning 时读取。

| 字段 | 说明 |
| --- | --- |
| `host` / `port` / `ws_path` | 音频运行时 WebSocket 监听地址、端口和路径 |
| `device_sample_rate` | 声卡播放采样率，当前统一音频链路要求 `48000` |
| `capture_sample_rate` | 送入唤醒词和 ASR 的采样率，当前要求 `16000` |
| `frame_ms` | 音频帧长度，当前统一链路要求 `10` 毫秒 |
| `render_queue_ms` | 播放队列容量；数字人/语音播放卡顿时可适当增大 |
| `capture_queue_ms` | 采集队列容量；过小会丢帧，过大则增加延迟 |
| `capture_stall_timeout_ms` | 采集线程无数据时判定声卡异常的超时时间 |
| `aec_tail_ms` | 回声消除尾部保持时间，外放场景建议不低于 `200` |
| `barge_in_min_speech_ms` / `barge_in_hangover_ms` | 插话检测最短语音和结束挂起时间 |
| `vad_aggressiveness` | VAD 严格程度，取值 `0..3`，越大越不容易误触发 |
| `input_device` / `output_device` | 声卡设备名称或编号；`null` 使用系统默认设备 |
