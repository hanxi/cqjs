# cqjs

基于 [quickjs-ng](https://github.com/nicbarker/quickjs) 的纯 C JavaScript 运行时，是 [goqjs](https://github.com/hanxi/goqjs) 的 C 等价实现。通过 stdin/stdout JSON Lines 协议与宿主进程交互，提供 Web API polyfill，无需 Go/CGO 依赖。

## 特性

- **纯 C 实现**: 无 Go/CGO 依赖，单文件编译，体积约 1MB
- **quickjs-ng 引擎**: 支持 ES2023+，活跃维护的 QuickJS 分支
- **Web API Polyfill**: console、fetch、crypto、zlib、Buffer、URL、Timer、TextEncoder/TextDecoder
- **lx 对象**: JS 层实现的 `globalThis.lx` 对象，兼容 LX Music Desktop API
- **JSON Lines 协议**: 与 goqjs 完全兼容的 stdin/stdout 交互协议
- **64MB 大栈**: 所有 JS 调用（eval/call/timer/fetch 回调）均在 64MB pthread 栈上执行，避免深度递归栈溢出
- **mbedTLS 加密**: MD5、AES-128-ECB、RSA-OAEP 加密，随机数生成

## 项目结构

```
cqjs/
├── main.c              # 主程序（事件循环、JSON Lines 协议、dispatch tracker）
├── cqjs.h              # 公共头文件（类型定义、函数声明）
├── Makefile             # 构建脚本
├── quickjs/             # QuickJS-ng 源码（amalgamated）
├── polyfill/            # Web API Polyfill（纯 C 实现）
│   ├── polyfill.c/h     # 统一注入入口
│   ├── console.c        # console.log/warn/error/info/debug/trace
│   ├── fetch.c          # fetch() → libcurl + pthread
│   ├── timer.c          # setTimeout/setInterval/clearTimeout/clearInterval
│   ├── crypto.c         # crypto.md5/aesEncrypt/rsaEncrypt/randomBytes (mbedTLS)
│   ├── buffer.c         # Buffer.from/isBuffer/toString + atob/btoa
│   ├── encoding.c       # TextEncoder/TextDecoder
│   ├── url.c            # URL/URLSearchParams
│   └── zlib.c           # zlib.inflate/deflate
├── js/
│   └── lx_prelude.js    # globalThis.lx 对象定义
├── test_lxsource.sh     # 洛雪音乐源加载测试
└── test_callMusicUrl.sh # callMusicUrl 端到端测试
```

## 依赖

- C 编译器（gcc/clang）
- libcurl（fetch polyfill）
- zlib（zlib polyfill）
- mbedTLS 4.x（crypto polyfill）
- pthread（定时器、大栈线程）

### macOS

```bash
brew install mbedtls curl zlib
```

### Ubuntu/Debian

```bash
sudo apt install libcurl4-openssl-dev zlib1g-dev libmbedtls-dev
```

## 编译

```bash
cd cqjs
make        # 编译生成 cqjs 可执行文件
make clean  # 清理构建产物
```

## 运行

```bash
# 执行 JS 代码
echo '{"id":"1","type":"eval","code":"1+1"}' | ./cqjs

# 执行 JS 文件
echo '{"id":"1","type":"eval_file","path":"script.js"}' | ./cqjs

# 带参数运行
./cqjs --memory-limit 128 --stack-size 8

# 交互模式（stdin JSON Lines）
./cqjs
```

## JSON Lines 协议

与 goqjs 完全兼容。

### 请求格式（stdin）

```json
{"id":"1", "type":"eval", "code":"1+1", "filename":"test.js"}
{"id":"2", "type":"eval_file", "path":"/path/to/script.js"}
{"id":"3", "type":"dispatch", "event":"request", "data":"{\"action\":\"search\"}"}
{"id":"4", "type":"callMusicUrl", "source":"wy", "songInfo":"{\"name\":\"歌曲\",\"singer\":\"歌手\",\"songmid\":\"123\"}", "quality":"320k"}
{"id":"5", "type":"exit"}
```

### 响应格式（stdout）

```json
{"id":"1", "type":"result", "value":"2"}
{"id":"2", "type":"error", "message":"SyntaxError: ..."}
{"type":"event", "name":"inited", "data":{"sources":{}}}
```

### 请求类型

| 类型 | 说明 |
|------|------|
| `eval` | 执行 JS 代码，返回结果 |
| `eval_file` | 执行 JS 文件，返回结果 |
| `dispatch` | 调用 `lx._dispatch(event, data)` → 等待 Promise 结果（30s 超时） |
| `callMusicUrl` | 调用 `lx._dispatch("request", {source, songInfo, quality})` → 等待结果（60s 超时） |
| `exit` | 退出进程 |

## Web API Polyfill

| API | 说明 |
|-----|------|
| `console.log/warn/error/info/debug/trace` | 输出到 stderr |
| `setTimeout/clearTimeout` | 定时器 |
| `setInterval/clearInterval` | 周期定时器 |
| `fetch(url, options)` | HTTP 请求（libcurl），返回 Promise |
| `crypto.md5/aesEncrypt/rsaEncrypt/randomBytes` | 加密工具（mbedTLS） |
| `zlib.inflate/deflate` | 压缩/解压，返回 Promise |
| `Buffer.from/isBuffer/toString` | Buffer 操作 |
| `atob/btoa` | Base64 编解码 |
| `TextEncoder/TextDecoder` | 文本编码 |
| `URL/URLSearchParams` | URL 解析 |

## lx 对象

`globalThis.lx` 在 JS 层实现（`js/lx_prelude.js`），提供：

- `lx.request(url, options, callback)` - HTTP 请求（基于 fetch，回调式 API）
- `lx.send(eventName, data)` - 发送事件到 C 侧
- `lx.on(eventName, handler)` - 注册事件处理器
- `lx._dispatch(requestId, eventName, data)` - 内部方法，支持 Promise 异步结果回传
- `lx._getSources()` - 获取已注册的音乐源列表
- `lx.utils.crypto.*` - 加密工具
- `lx.utils.zlib.*` - 压缩工具
- `lx.utils.buffer.*` - Buffer 工具

## 与 goqjs 的对比

| 特性 | goqjs | cqjs |
|------|-------|------|
| 语言 | Go + CGO | 纯 C |
| 二进制大小 | ~10MB | ~1MB |
| 依赖 | Go 工具链 | C 编译器 + libcurl + mbedTLS |
| 交叉编译 | 受 CGO 限制 | 标准 C 交叉编译 |
| JSON Lines 协议 | 完全兼容 | 完全兼容 |
| Web API Polyfill | Go 实现 | C 实现 |
| 栈溢出保护 | 64MB pthread | 64MB pthread |

## 测试

```bash
# 测试加载洛雪音乐源脚本
bash test_lxsource.sh

# 测试 callMusicUrl 端到端
bash test_callMusicUrl.sh
```

## 许可证

[MIT](LICENSE)
