# 测试脚本

这些脚本直接构造 DNS 报文或 TCP 隧道，用来验证解析器与代理，不依赖第三方 npm 包（只使用 Node.js 内置模块）。

## 准备

```bat
:: 在项目根目录启动一个测试实例（普通用户即可，端口 5451 不冲突）
dnspilot.exe --config build\test.json --port 5451 --http-port 8765 --proxy-port 8899 --no-browser --verbose
```

首次启动会生成 `build\test.json`；示例规则可在面板里添加，或用 API：

```bat
curl -X POST -H "Content-Type: application/json" -H "X-DNSPilot: 1" ^
  -d "{\"op\":\"add\",\"rule\":{\"match\":\"example.com\",\"profile\":\"cloudflare\"}}" ^
  http://127.0.0.1:8765/api/rules/domain
```

## 脚本

| 脚本 | 作用 | 期望 |
| --- | --- | --- |
| `node q.cjs example.com 5451` | 用系统解析器查询一次 A 记录 | 输出 IP 数组 |
| `node dnstest.cjs 5451 example.com` | UDP → 等 1.6s → UDP → TCP → 畸形 5 字节报文 | 第 2 次 TTL 比第 1 次小；TCP 有应答；畸形报文 `rcode=1` |
| `node connecttest.cjs example.com 443 8899` | HTTP CONNECT 隧道内做真实 TLS 握手并 GET | `CONNECT resp: HTTP/1.1 200`、`TLS OK`、`HTTP: HTTP/1.1 200` |
| `node load.cjs 100 5451` | 100 个并发 UDP 查询 | `{"sent":100,"received":100,...}`，退出码 0 |

## 代理测试（curl）

```bat
curl --proxy socks5h://127.0.0.1:8899 https://iana.org/ -o nul -w "%{http_code}\n"
curl --proxy http://127.0.0.1:8899 https://example.com/ -o nul -w "%{http_code}\n"
```

## 接管相关（需要管理员）

```bat
dnspilot.exe --takeover-plan      :: 只打印计划
dnspilot.exe --elevate --takeover :: UAC 提权后接管
dnspilot.exe --restore            :: 异常退出后恢复
```
