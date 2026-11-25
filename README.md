# hk-snapshot-service

海康 NVR 截图 HTTP 服务（C++17，同步接口，Docker 部署）。

| 模式 | 实现路径 | 适用 |
|---|---|---|
| **实时截图** | `NET_DVR_CaptureJPEGPicture_NEW`（设备侧直接出 JPEG，不经本地解码） | 当前画面 |
| **历史截图** | 默认从查询时间至后 3 秒拉回放流，解码至目标时间并返回已观察到的最近帧 | 任意历史时间点 |

解码链路的关键设计：编码类型只信 SDK 的 `byVideoEncType` 元数据（绝不从 NAL 猜测）、用 `NET_DVR_PACKET_INFO_EX` 帧级日历字段做 OSD 配对、以 PTS 索引解决 B 帧重排导致的输出乱序、MJPEG 编码含 limited→full 色彩范围展开。

启动后访问 `http://localhost:8180/`，可通过网页获取和保存实时或历史截图。

## API

截图通过 POST 请求同步生成并直接返回 JPEG。`/api/snapshots` 和
`/api/v1/capture` 为兼容路径，新接入建议使用 `/snapshots`。

### `POST /snapshots`

请求体（JSON）：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `host` | string | ✔ | NVR 地址 |
| `port` | int | | SDK 端口，默认 `8000` |
| `username` / `password` | string | ✔ | 设备账号 |
| `channel` | int | | NVR 通道号（IP 通道按设备实际编号，通常从 33 起），默认 `1` |
| `time` | string / null | | **缺省/null = 实时截图**；传 NVR 本地时间即历史截图：`YYYY-MM-DD HH:MM:SS`（也接受 `T` 分隔 / 尾部 `Z`） |
| `stream_type` | int | | 历史回放码流：`0` 主码流（默认，画质最好）`1` 子码流 `2` 三码流 |
| `timeout_ms` | int | | 单次截图整体超时，默认 `15000`，`1000-60000` |
| `jpeg_quality` | int | | `0` 最好 `1` 较好（默认）`2` 一般 |
| `picture_size` | int | | 实时抓图分辨率档（海康语义），默认 `255`（0xff 跟随当前码流） |

响应：

- 成功 `200`：`Content-Type: image/jpeg`，图片即响应体。附加头：
  - `X-Capture-Mode`: `realtime` / `history`
  - `X-Capture-Osd-Time`: 匹配帧的 OSD 时间（历史模式）
  - `X-Capture-Frame-No`、`X-Capture-Resolution`、`X-Capture-Elapsed-Ms`
- 失败：JSON `{"error":{"code":"...","message":"..."}}`

| HTTP | code | 含义 |
|---|---|---|
| 400 | `invalid_param` / `invalid_json` | 请求参数非法 |
| 401 | `login_rejected` | 设备账号密码错、权限不足 |
| 404 | `no_recording` / `frame_not_found` | 回放无录像 / 查询时间前 5 秒至后 3 秒内无可匹配帧 |
| 429 | `too_many_requests` | 同一设备截图并发请求达上限（`HKMEDIA_MAX_CONCURRENT`） |
| 501 | `unsupported_codec` | 通道编码非 H.264/H.265，或 FFmpeg 无对应解码器 |
| 502 | `login_failed` / `sdk_capture_failed` / `playback_*` / `decode_failed` 等 | 设备/SDK 交互失败（message 含 SDK 错误码） |
| 504 | `capture_timeout` / `playback_no_data` | 超时未命中 / 回放无数据 |
| 500 | `internal_error` | 服务内部错误 |

### `GET /healthz`

存活检查，返回 `200 ok`。

## 示例

```bash
# 实时截图
curl -sS -X POST http://localhost:8180/snapshots \
  -H 'Content-Type: application/json' \
  -d '{"host":"192.168.1.64","username":"admin","password":"xxx","channel":33}' \
  -o now.jpg

# 历史时间点截图（NVR 本地时间）
curl -sS -X POST http://localhost:8180/snapshots \
  -H 'Content-Type: application/json' \
  -d '{"host":"192.168.1.64","username":"admin","password":"xxx","channel":33,
       "time":"2026-11-19 10:00:05"}' \
  -o frame.jpg -D headers.txt
# headers.txt 中 X-Capture-Osd-Time 为命中帧的 SDK 日历时间。
# 画面叠加时间可能与 SDK 日历时间不同。
```

## 构建与部署

```bash
docker compose up -d --build
```

镜像：构建阶段用 ubuntu 24.04（noble）+ 系统 FFmpeg dev（noble 为 FFmpeg 6.1，
纯软解码，无需 GPU）；运行阶段只带 FFmpeg 运行库（libavcodec60/libavutil58/
libswscale7）+ `third_party/HCNetSDK/lib` 全目录（保持
`libhcnetsdk.so` 与 `HCNetSDKCom/` 的相对布局），非 root 用户运行。

本地（Linux）构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/hk-snapshot-service   # 依赖 RPATH 已指向 third_party/HCNetSDK/lib
```

### 环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `HKMEDIA_LISTEN_ADDR` / `HKMEDIA_PORT` | `0.0.0.0` / `8180` | 监听地址/端口 |
| `HKMEDIA_MAX_CONCURRENT` | `16` | 每个设备（按请求的 host 和 port 区分）的截图并发上限，超出返回 429；HTTP 线程池仍由所有设备共享 |
| `HKMEDIA_DEFAULT_TIMEOUT_MS` | `15000` | 默认整体超时 |
| `HKMEDIA_JPEG_QUALITY` | `1` | 默认 JPEG 档位 |
| `HKMEDIA_CONNECT_TIMEOUT_MS` | `5000` | SDK 连接/收流超时 |
| `HKMEDIA_LOG_LEVEL` | `info` | `debug/info/warn/error` |
| `APPNAME` | `hk-snapshot-service` | spdlog logger 名与日志文件名前缀 |
| `LOGDIR` | `/data/logs` | 滚动日志目录（100MB x 30，不可写回退 `/tmp/hk-snapshot-service/logs`） |
| `HKMEDIA_SDK_LOG` | `0` | `1` = 开启 HCNetSDK 自身日志（排障） |

### 请求排障日志

日志按 `时间|-|服务名|-|"源码文件:行号"|-|级别|-|函数名|-|消息` 输出。
`log_debug/info/warn/error(component, message)` 在调用处采集源码位置，消息保留请求 ID 和组件名。

每个 HTTP 请求生成独立的 `request_id`，通过响应头 `X-Request-Id` 返回；HTTP 处理、
SDK 登录、回放、ES 回调和截图日志使用同一个 ID。设备用户名、密码按输入
原样记录，不做脱敏。请求头和原始请求体按 JSON 转义为单行，方便按请求检索。

默认 `info` 级别包含：

- `request.start` / `request.body`：请求方法、完整 URL、来源地址/端口、全部请求头及原始请求体。
- `capture.params`：参数校验通过后实际生效的全部业务参数，包括默认值、密码、
  查询时间、码流和超时。
- SDK 阶段：登录开始/完成、错误码、设备通道信息、编码查询及回退原因、实际回放窗口、
  回放句柄、首帧元数据、进度变化、停止回放和注销结果。
- `capture.result`：JPEG 大小、分辨率、帧号、OSD 和业务耗时。
- `response.ready` / `request.complete`：实际响应状态、全部响应头、文本/JSON 响应体、
  输出字节数以及处理/总耗时。参数错误、并发拒绝、404 和请求体过大同样记录。

`info` 及以上日志及时刷新到文件。设置 `HKMEDIA_LOG_LEVEL=debug` 后，还会记录 I 帧及其他每帧 ES 的
帧号、SDK PTS、OSD、帧类型、包模式、大小和分辨率；FFmpeg 警告默认进入服务日志。

```bash
# 从响应头获取 X-Request-Id 后，查看该请求的完整处理过程。
grep -F 'request_id=响应头中的ID' /data/logs/hk-snapshot-service.log

# 日志接口回归测试：自动启动隔离服务，使用本地模拟连接，不需要真实 NVR。
python3 tests/request_logging_test.py --binary build/hk-snapshot-service
```

## 设计要点

### 历史截图为什么这样找帧

1. `PlayBackByTime_V40` 默认从目标时间回放至后 3 秒；设备可能返回起点之前的关键帧；请求时间仅支持秒精度，匹配仍使用帧的毫秒级 OSD 时间；
2. 全速软解（FFmpeg `thread_count=0` 自动多线程），解码直至目标时间；
3. SDK 回调线程只做深拷贝入队；解码线程按 `PTS ↔ best_effort_timestamp` 索引把
   每个解码输出配回它的 `NET_DVR_PACKET_INFO_EX` 日历字段（B 帧重排后输出顺序
   ≠ 回调顺序，不能按到达顺序配对）；
4. 比较已配对帧的 SDK OSD 与查询时间的绝对差；等距时选择较早的帧。解码输出首次到达或超过目标时间后，立即选择已观察到的最近帧并返回；
5. 如果流提前结束，则冲刷解码器并选择已观察到的最近帧；窗口内没有可匹配帧时返回 404。

提前返回依赖解码输出的 OSD 时间基本递增；如果设备 OSD 回退，后续未解码帧可能更接近目标时间。

处理耗时取决于设备回放、窗口大小、码率和软件解码速度。

### 回放接收与控制

- 回调全速接收，队列不限制容量，解码到目标时间后立即返回，不做流控 pacing；
- 以 `NET_DVR_PLAYGETPOS`（0=进行中 100=结束 200=错误）跟踪进度，进度 100% 后
  链路静默达到 `eof_quiet_ms` 判定 EOF，期间每 2 秒 `NET_DVR_KEEPALIVE` 心跳保活；
- SDK ES 包为 FU-A 分片模式（`packet_mode=1`）直接报错；通道为海康私有 264 时
  日志告警并按标准 H.264 处理。

不依赖 NVR 的回归测试：

```bash
cmake -S . -B build -DHKMEDIA_BUILD_TESTS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

### 时间语义

查询 `time` 与匹配都基于 **NVR 设备本地时间**（OSD 字段原样比较，不做时区换算）。
跨时区部署时注意：NVR 时间 ≠ 容器 UTC 时间，请调用方传 NVR 侧时间。

## 目录

```
src/
  main.cpp                         程序入口与启动、关闭顺序
  common/                          公共基础，不依赖业务模块
    app_log.hpp                    日志初始化与日志接口
    error.hpp                      统一服务错误
    request_context.hpp            请求 ID 与回调线程上下文
    time_util.hpp                  设备日历时间解析与毫秒换算
  config/
    config.{hpp,cpp}               环境变量与配置校验
  model/                           纯数据类型，不依赖 HTTP、SDK、FFmpeg
    capture_types.hpp              截图请求与结果
  http/
    http_api.{hpp,cpp}             路由、参数校验、并发限制、响应与错误映射
    request_log.{hpp,cpp}           HTTP 生命周期、输入输出与结构化排障日志
  sdk/                             海康 SDK 接入与 ES 数据传递
    sdk_env.hpp                    SDK 生命周期与登录会话
    channel_query.hpp              通道压缩配置查询
    sdk_codec.hpp                  SDK 编码类型映射
    es_queue.hpp                   ES 帧数据与队列
    es_log.hpp                     首帧、I 帧及逐帧 ES 排障元数据
  capture/
    realtime_capture.{hpp,cpp}     设备实时截图
    history_capture.{hpp,cpp}      回放、解码、匹配历史截图
    nearest_osd.hpp                搜索窗口内最近的 SDK 时间
  media/
    jpeg_encoder.{hpp,cpp}         JPEG 编码与像素格式、色彩范围转换
    ff_util.hpp                    FFmpeg 错误处理工具
tests/
  nearest_osd_test.cpp              最近 OSD 帧选择回归测试
  request_context_test.cpp          跨线程请求上下文回归测试
  request_logging_test.py           HTTP 日志、错误分支与并发关联测试
  test.sh                          真实设备手动调用示例
scripts/
  smoke_test.sh                    HTTP 接口冒烟测试
third_party/
  HCNetSDK/                        第三方 SDK 原始头文件与 Linux x86_64 库
```

调用关系为 `main → http → capture → sdk / media`；`common`、`config`、
`model` 提供共享基础。SDK 接入层不依赖 HTTP 层或业务请求模型。

nlohmann/json（`nlohmann-json3-dev`）与 cpp-httplib（`libcpp-httplib-dev`）使用
系统 apt 包（见 Dockerfile），不随仓库携带；本地构建时先
`apt-get install nlohmann-json3-dev libcpp-httplib-dev`。注意 apt 版 cpp-httplib
为编译库，运行环境需同时安装对应的 `libcpp-httplib0.14t64` 运行库。
