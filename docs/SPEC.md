# DNS Pilot 规格（SPEC）

本文件是实现的唯一产品规则来源。实现与测试必须以本文件为准。

## 1. 目标

在同一台 Windows 机器上，让**指定的应用**或**指定的域名（网站）优先使用指定的 DNS 服务器**，并提供一个可视化面板展示实时解析流量、命中规则、上游服务器、延迟与缓存效果。

- 交付物：单文件原生程序 `dnspilot.exe`（C++17，无运行时依赖）+ 内嵌 Web 面板 + 配置 JSON。
- 不依赖内核驱动、不注入进程、不修改应用。

## 2. 绑定路径（产品核心）

Windows 的 DNS 解析有两个事实，决定了必须同时提供多条路径：

1. 应用把查询交给系统 DNS（`127.0.0.1:53`）时，通常由 Windows DNS Client 服务（`svchost.exe` / Dnscache）代发，源端口无法归属到具体应用。因此"按域名绑定"在这条路径上 100% 可靠，"按应用绑定"只能尽力归因。
2. 自带解析器的应用（Chrome/Edge/Firefox、部分游戏与工具）或显式指定 DNS 的应用会自己发包，源端口可经 `GetExtendedUdpTable` 归属到 PID，因此"按应用绑定"可以直接生效。
3. 浏览器另有自己的配置文件夹，可写入其原生安全 DNS（DoH）设置，从而按浏览器 profile 单独绑定（不依赖本机解析器）。

因此产品提供：

| 路径 | 生效方式 | 权限 | 覆盖范围 |
| --- | --- | --- | --- |
| **域名绑定** | 接管系统 DNS 到本机解析器（UDP/TCP 53） | 管理员 | 全系统，透明 |
| **应用绑定（直连）** | 解析器按查询源端口归因 PID → exe → 规则 | 管理员（需接管） | 自带解析器的应用 |
| **应用绑定（代理）** | 应用把 HTTP/SOCKS 代理指向本机，代理按连接源 PID 归因，用该应用的 DNS 配置解析目标域名 | 普通用户 | 支持代理的应用（浏览器等），无需接管 |
| **浏览器配置绑定** | 写入 Chrome/Edge `Preferences` 或 Firefox `prefs.js` 的安全 DNS（DoH） | 普通用户 | 该浏览器 profile |

## 3. 术语与数据模型

- **Profile（DNS 服务器配置）**：`id`、`name`、`servers[]`（`ip` 或 `ip:port`，IPv4/IPv6）、`strategy`（`failover` 主用+失败回退；`only` 仅用第一项）、`builtin`。
- **内置 provider 名**（`provider` 字段，用于根据地区自动填服务器，且便于说明）：
  `system`（沿用系统原 DNS）、`cloudflare`、`aliyun`、`google`、`dnspod`、`quad9`。
- **AppRule**：`match`（exe 文件名，支持 `*`/`?` 通配，大小写不敏感）、`profile`、`enabled`、`note`。
- **DomainRule**：`match`（域名后缀，如 `netflix.com` = `netflix.com` 及其子域；支持 `*.x.com`；仅写 `x.com` 等价）、`profile`、`enabled`、`note`。
  - `match` 在保存和加载时归一化：允许直接粘贴网址（`https://host:port/path?x#y` → `host`），统一小写、去掉末尾点；匹配逻辑对规则与查询名都做同样的归一化，因此旧配置中存成完整 URL 的规则同样生效。
- **Config**：`version`、`resolver{listen,port,httpPort,cacheEnabled,minTtl,maxTtl,queryTimeoutMs}`、`proxy{enabled,port}`、`profiles[]`、`defaultProfile`、`appRules[]`、`domainRules[]`、`takeover{active,backup[]}`、`ui{openBrowser,restoreOnExit,nativeWindow,theme}`。
  - `ui.theme` 取值 `"light"` / `"dark"`，默认 `"light"`；字段缺失或非法时按 `"light"` 处理（旧配置迁移）。
- **编码与 BOM**：配置文件与导入文件按 UTF-8 JSON 读取；允许 UTF-8 BOM，以及 UTF-16 LE/BE BOM（读取时自动转换为 UTF-8），便于用记事本/PowerShell 编辑后直接使用。解析失败时回退默认配置并报告错误；原文件保持不变，直到下一次保存配置。
- **规则专用 Profile**：规则勾选自定义服务器时自动创建，`provider="auto"`，名称 `<match> · 专用`；规则不再引用时自动清理。
- **BrowserProfile（不持久化）**：`browser`、`browserName`、`folder`、`path`、`name`、`email`、`running`、`mode`（off/secure/automatic/unknown）、`dohUrl`；读取自浏览器配置文件，写入时直接修改该文件。
- **Event**：`t`、`source`（`dns`/`proxy`）、`client`（exe 名或 `未知`）、`pid`、`domain`、`qtype`、`profileId`、`profileName`、`upstream`、`latencyMs`、`cached`、`ruleType`（`domain`/`app`/`none`）、`ruleMatch`、`status`（`ok`/`nxdomain`/`servfail`/`timeout`/`blocked`/`error`）。

## 4. 状态所有者（唯一写入路径）

| 状态 | 所有者 | 持久化 |
| --- | --- | --- |
| 配置（规则、Profile、端口、接管备份） | `ConfigStore` | `dnspilot.json`（exe 同目录，不可写时 `%APPDATA%\DNSPilot\`） |
| DNS 缓存 | `DnsCache`（内存） | 不持久化 |
| 统计与事件环（最近 500 条） | `Stats`（内存，互斥锁保护） | 不持久化 |
| 系统 DNS 接管状态 | `SystemDns`（`takeover.active` + `backup[]`） | 配置内备份；变更前必须已成功监听 |

桌面界面与 Web 面板都不持有业务状态，二者都通过 `ConfigStore` / `Engine` 的公开方法读写；`ConfigStore` 是配置的唯一写入点。

## 5. 规则优先级

1. `DomainRule`（最长后缀优先）
2. `AppRule`（按数组顺序，首个匹配）
3. `defaultProfile`

代理路径：目标域名先匹配 `DomainRule`，再匹配连接方 exe 的 `AppRule`，否则 `defaultProfile`。DNS 路径的默认 profile 为 `defaultProfile`（默认 `system`）。

内置 `system` profile 的服务器在启动时从网卡自动探测（排除 `127.0.0.1`/`::1`），探测不到时回退 `223.5.5.5`。接管后重新探测时必须排除本机地址，避免自环。

## 6. 解析管线（DNS）

```
client → 解析问题(qname,qtype,qclass) → 命中规则得到 profile
      → 缓存命中（key=qname|qtype|qclass|profile）→ 回写递减 TTL → 返回
      → 依 profile 顺序请求上游（UDP，超时 queryTimeoutMs；窗口内最多重传 2 次；响应 TC=1 时改 TCP 重试）
      → 全部失败返回 SERVFAIL；成功则按最小 TTL 缓存（clamp [minTtl,maxTtl]，负响应取 SOA TTL/30s）
      → 统计 + 事件入环
```

- 必须保留客户端原始报文 ID；上游使用独立随机 ID，拒绝 ID/源地址不匹配的响应。
- 缓存命中时递减应答/权威/附加段 RR 的 TTL（最低 0），过期视为未命中。
- 不缓存 `SERVFAIL`、截断响应。
- 上游地址不得为本机监听地址（防环），配置写入时校验。
- 失败的服务器进入短时退避（5s/10s/15s…上限 30s），退避期内排到列表末尾，但仍是最后可用手段。

## 7. 代理管线（按应用 DNS）

- SOCKS5（无认证，CMD=CONNECT，ATYP=域名/IPv4/IPv6）+ HTTP `CONNECT`；不支持明文 HTTP 转发（返回 501 并说明）。
- 收到 CONNECT 时用 `GetExtendedTcpTable` 归属客户端 PID → exe → 规则 → 用对应 profile 解析目标域名（A/AAAA，优先 A）→ 连接 → 双向转发。
- 解析结果必须产生一条 `source=proxy` 的事件，可视化中与 DNS 事件同流展示。
- 只监听 `127.0.0.1`（及 `::1`）。

## 8. 系统 DNS 接管与恢复

- 顺序：先成功绑定解析器端口 → 再枚举 Up 且有 IPv4 地址的网卡 → 保存原状态（静态/DHCP + 服务器列表）→ `netsh` 设置 `127.0.0.1` / `::1` → `ipconfig /flushdns`。
- 任一网卡设置失败：继续尝试其余网卡，最终报告失败项；若全部失败则自动恢复并返回错误。
- 恢复：按备份逐网卡恢复（DHCP 或静态列表），`flushdns`；成功后清除 `takeover.active` 与备份。
- 正常退出（Ctrl+C / 关闭控制台 / API 退出）且 `restoreOnExit=true` 时自动恢复。
- 未以管理员运行：接管 API 返回 `403` 与明确提示；可从面板触发提权重启（`POST /api/elevate`，`ShellExecuteW(runas)` 启动新实例，当前实例退出；新实例绑定端口时重试 5s）。
- `--takeover-plan` 只打印网卡、原配置与将执行的 netsh 命令，不修改系统，用于管理员执行前的核对。
- 接管前必须自检：解析器对 `example.com` 的探测查询成功，否则拒绝接管（防止断网）。

## 9. 原生桌面界面（默认入口）

- 单进程 Win32 + GDI+ 自绘窗口，不嵌入 WebView / Electron；`ui.nativeWindow=true` 时启动即打开。
- 左侧导航：总览、应用规则、域名规则、DNS 服务器、浏览器绑定、系统接管、设置。
- 总览：KPI 卡片、60 秒原生图表、实时解析流、热门域名/应用；每 700ms 从 `Stats::view()` 刷新。
- 应用规则编辑器：
  - **进程选择器**（OBS 风格）：枚举当前所有进程与可见窗口标题，显示图标、进程名、标题、PID、路径；支持搜索、刷新、双击/Enter 确认；返回 exe 名称。
  - 也可手动输入 exe 名称（支持 `*`/`?`）。
  - DNS 配置下拉（原生列表）+「使用自定义 DNS 服务器」开关；开关打开时保存会创建该规则专用 Profile。
  - 规则支持启用/停用、编辑、删除；置灰规则不参与匹配。
- 域名规则编辑器：域名后缀 + DNS 配置/自定义服务器。
- DNS 服务器页：新增、编辑、删除配置；服务器列表逗号分隔；策略（主用+回退 / 仅主用）；`system` 只读。
- 浏览器绑定页：扫描 Chrome/Edge/Firefox 配置；显示 profile 名称/邮箱/当前 DoH/运行状态；绑定（写 `dns_over_https` 或 `prefs.js` TRR）与恢复默认；运行中的浏览器禁止写入。
- 系统接管页：接管/恢复、提权重启、网卡 DNS 列表。
- 设置页：默认配置、缓存、TTL、超时、代理、端口、退出恢复、桌面窗口开关、界面主题（浅色/深色）、配置导出/导入。
- 绑定查询页（只读）：
  - **按程序**：从运行中的进程选择一个 exe，显示命中的应用规则（或「默认配置」）、生效的 DNS 配置、服务器列表与策略。
  - **按域名**：输入域名（支持子域；可粘贴 URL，自动取主机名），显示命中的域名规则（最长后缀优先，或「默认配置」）、生效配置、服务器与策略。
  - 规则展示包含规则类型与匹配内容；域名规则优先于应用规则，未命中时回退到 `defaultProfile`。
  - 仅做规则匹配与配置查询，不写入配置、不触发解析请求；`system` 配置显示当前探测到的网卡 DNS。
- 窗口自绘标题栏（最小化/最大化/关闭、拖动、边缘缩放）；关闭窗口等价于退出程序（按设置恢复系统 DNS）。
- `--no-gui` 时仅运行服务与 Web 面板；从终端启动并进入 GUI 前调用 `FreeConsole()`，避免终端关闭连带退出。

### 9.1 主题（浅色 / 深色）

- 默认浅色（白底）；深色主题保留。所有颜色集中在 `ui_native.cpp` 的调色板变量中，由 `applyTheme(bool)` 整体切换，绘制代码不直接硬编码表面色。
- 切换入口：
  1. 自绘标题栏的主题按钮（太阳/月亮，位于最小化左侧），单击立即切换；
  2. 设置页「修改设置」→「界面主题」列表（浅色/深色），保存后立即生效。
- 持久化：写入 `ui.theme`（经 `ConfigStore`，唯一写入点）；启动时读取，旧配置缺字段按浅色。
- 覆盖范围：
  - 主界面全部 7 页（侧边栏、标题栏、页头胶囊、卡片、表格斑马纹、badge、说明框、接管状态卡、设置分隔线、toast）；
  - 总览 60 秒图表（背景、网格、柱渐变、文字、空状态）；
  - 表单对话框（背景、输入框、列表选中、开关、主/次按钮、滚动裁剪、系统标题栏的 DWM 深/浅色与文字色）；
  - OBS 式进程选择器（窗口/标题栏、搜索框、ListView 背景与斑马纹、选中行、滚动条、按钮、图标占位、DWM 标题栏、ListView 主题）；
  - 浏览器绑定页（信息框、浏览器 badge、运行状态 badge、绑定/恢复按钮的启用与禁用态）。
- 浅色强调色按可读性微调（如 `#0B7F96`、`#0F7A57`、`#A6660F`），避免白底上的低对比文字。
- 标题栏按钮、表单输入框与开关的命中区不因主题变化；切换后不重建窗口，仅重建颜色相关 GDI 画刷并重绘。

### 9.2 配置导出 / 导入

- 入口：设置页「导出配置…」「导入配置…」；命令行 `--export-config <path>` / `--import-config <path>`（执行后直接退出，不启动服务）。
- 导出内容：`profiles[]`（含自定义与规则专用配置）、`appRules[]`、`domainRules[]`、`defaultProfile`。
  - 不导出：解析/面板/代理端口、缓存与超时、接管状态与网卡备份、浏览器绑定、主题等本机状态。
- 文件格式（UTF-8 JSON）：
  ```json
  { "format": "dnspilot-export", "version": 1, "appVersion": "1.2.0",
    "defaultProfile": "system", "profiles": [], "appRules": [], "domainRules": [] }
  ```
- 导入采用**合并**语义，不删除现有配置：
  - 配置按语义（名称 + 服务器 + 策略 + provider）去重，已存在则复用本机配置；
  - `provider=system` 一律映射到本机 `system`，不覆盖本机探测到的服务器；
  - 配置 id 冲突时生成 `import-xxxx` 新 id，并重写引用它的规则与 `defaultProfile`；
  - 域名规则在导入时归一化；应用规则按 `match` 忽略大小写去重；重复规则与引用无效配置的规则计入“跳过”；
  - 导入后清理不再被引用的规则专用配置（`provider=auto`）。
- 结果反馈：界面弹窗显示“新增配置 / 复用配置 / 新增规则 / 跳过规则”数量；CLI 打印同样摘要。

## 10. HTTP API（仅 127.0.0.1，变更请求需 `X-DNSPilot: 1`）

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/` | 内嵌面板 |
| GET | `/api/state` | 配置摘要、统计、最近事件、网卡/接管/权限状态、版本 |
| GET | `/api/test?domain=&profile=` | 走完整管线解析一次（不写缓存），返回答案/IP/延迟/上游 |
| POST | `/api/rules/app` | `{op:add\|update\|delete\|toggle, rule}` |
| POST | `/api/rules/domain` | 同上 |
| POST | `/api/profiles` | `{op:add\|update\|delete, profile}`（`system` 不可删） |
| POST | `/api/settings` | 修改端口/缓存/默认 profile/代理开关（监听端口变更需重启，返回提示） |
| POST | `/api/takeover` | `{action:apply\|restore}` |
| POST | `/api/elevate` | 提权重启 |
| POST | `/api/cache/clear` | 清空缓存 |
| POST | `/api/quit` | 优雅退出 |

响应统一 `{"ok":true|false,"error":"..."}`，UTF-8；`/api/state` 每秒轮询即可支撑实时可视化。

## 11. 安全

- 仅绑定环回地址；变更接口要求 `X-DNSPilot` 头（阻断跨站简单请求伪造）。
- 上游/代理转发前校验 IPv4/IPv6 字面量与端口范围；不记录凭据与完整 URL（代理只记录主机名）。
- 不修改系统文件、不安装服务/驱动；接管只改网卡 DNS，保留完整备份。

## 12. 验收场景

1. **域名绑定**：`DomainRule example.com → aliyun`，`default → cloudflare`；查询 `example.com` 走上游 223.5.5.5，查 `www.iana.org` 走 1.1.1.1；面板事件显示 `ruleType=domain`。
2. **应用绑定（直连）**：`AppRule node.exe → aliyun`；node 直接向解析器查询时事件 `ruleType=app`、`client=node.exe`；域名规则优先级高于应用规则。
3. **缓存**：同一域名连续两次查询，第二次 `cached=true` 且 TTL 递减；`maxTtl`/`minTtl` 生效；`SERVFAIL` 不入缓存。
4. **优先级**：同时存在 `DomainRule netflix.com → A` 与 `AppRule node.exe → B`，node 查询 `netflix.com` 命中 A。
5. **上游回退**：profile 主服务器不可达（如 `127.0.0.1:9`）时在超时后自动使用备用并成功返回；延迟计入事件。
6. **代理**：`curl --proxy socks5h://127.0.0.1:8899 https://example.com` 成功返回 200；面板出现 `source=proxy`、`client=curl.exe` 事件；HTTP CONNECT 同样成功。
7. **接管（管理员）**：接管后 `Get-DnsClientServerAddress` 显示回环地址，外部域名仍可解析；恢复后回到原 DNS；未提权时接口明确返回 403 且不改系统。
8. **可视化**：面板展示实时查询流、命中规则、上游、延迟、缓存命中率、按应用/域名排行、延迟曲线、规则增删改；1 秒内刷新。
9. **健壮性**：畸形报文返回 FORMERR 不崩溃；端口被占用时给出明确错误；退出时恢复 DNS。
10. **并发**：100 个并发 UDP 查询全部返回（无丢应答），无崩溃；上游丢包时在超时窗口内重传并成功。

11. **原生界面**：启动即打开桌面窗口；进程选择器列出运行进程，可搜索并双击选中并回填；规则可保存/启停/删除。
12. **规则单独绑定**：勾选自定义服务器保存后生成专用配置，规则事件显示该配置；删除规则后专用配置被清理。
13. **浏览器配置绑定**：对测试 profile 写入 Chrome `dns_over_https` / Firefox TRR，界面显示 DoH 地址；运行中的浏览器被拒绝；备份文件存在且内容为原文件。
14. **桌面窗口生命周期**：从终端启动后关闭终端，程序仍存活；关闭窗口按设置恢复系统 DNS 并退出。
15. **主题**：
    - 新配置默认浅色；旧配置（无 `ui.theme`）启动后为浅色；
    - 标题栏主题按钮单击后主界面立即切换，`ui.theme` 同步更新；设置页列表切换并在保存后立即生效；
    - 重启后主题保持；
    - 两套主题下，总览图表、表单对话框、进程选择器、浏览器绑定页、系统接管页均无白字白底/黑字黑底，输入框、选中行、按钮、badge 可辨认。
16. **绑定查询**：
    - 应用规则 `chrome.exe → Cloudflare`、域名规则 `netflix.com → 阿里云`、默认 `system`：
      - 按程序查 `chrome.exe` 显示「应用规则 · chrome.exe」、Cloudflare、`1.1.1.1, 1.0.0.1`；
      - 按域名查 `www.netflix.com` 显示「域名规则 · netflix.com」、阿里云、`223.5.5.5, 223.6.6.6`；
      - 查 `example.org` 显示「默认配置」与 `system` 当前探测到的网卡 DNS。
    - 粘贴 `https://www.netflix.com/title/1` 能识别主机名；未命中规则时明确显示回退到默认配置。
    - 两套主题下该页排版、文字对比正常；查询不修改配置。
17. **域名规则归一化**：
    - 在「添加域名规则」里直接粘贴 `https://tccc-adminlt.eyar.com/#/login` 保存，规则列表显示 `tccc-adminlt.eyar.com`；
    - 绑定查询输入同一网址，显示该域名规则与绑定的 DNS 配置/服务器；
    - 旧配置里 `match` 存成完整 URL 的规则，启动后无需手工修改即可命中，真实查询使用该规则绑定的上游。
18. **配置导出 / 导入**：
    - 导出的 JSON 含自定义配置与规则，不含端口、接管备份、主题；
    - 新配置导入后，自定义配置与规则出现，规则引用的配置 id 正确；
    - 重复导入不产生重复项（跳过计数增加）；
    - 目标已有同 id 但内容不同的配置时，导入生成 `import-xxxx` 并重写规则与 `defaultProfile`；
    - 被跳过的规则不会留下无引用的规则专用配置。
19. **BOM 兼容**：
    - 用记事本/PowerShell 保存的 UTF-8 BOM 配置或导入文件能正常解析，不出现“配置文件解析失败”；
    - UTF-16 LE/BE BOM 的同一文件同样能解析（内容一致）；
    - 无 BOM 的 UTF-8 文件行为不变。

## 13. 已知限制

- 明文 HTTP 代理仅支持 `CONNECT`（HTTPS）；HTTP 站点请用 SOCKS5。
- 系统 DNS 客户端代发的查询无法归因到具体应用（显示为 `svchost.exe`/未知），此时使用域名规则或代理路径。
- 仅支持 Windows 10/11 x64。
- 原生界面为自绘控件（列表、开关、滚动条），滚动条不是系统辅助功能控件。
- 主题切换仅覆盖原生桌面界面；可选的 Web 面板仍为深色。
- 浏览器原生只支持 DoH；需要普通 DNS 时请使用本机代理。
- 真实系统接管需要 UAC，当前环境无法在线验证，使用 `--takeover-plan` 核对命令。
