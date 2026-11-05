# 远程采集协议 v1

## 传输与启动

客户端为每台主机启动系统 `ssh`，分离 stdin、stdout、stderr，不申请 PTY。当前客户端使用以下命令形式：

```bash
ssh -T -o BatchMode=yes -o StrictHostKeyChecking=yes \
  -o ConnectTimeout=5 -o ServerAliveInterval=5 -o ServerAliveCountMax=2 \
  dev-01 /opt/btop-cluster/bin/btop-agent --stdio
```

首次登录认证和主机密钥确认由用户在部署时完成。认证失败显示 `AUTH_REQUIRED`，按 `r` 重试；不在 TUI 中弹密码输入框，不通过关闭主机密钥校验解决连接问题。[OpenSSH 手册](https://man.openbsd.org/ssh.1)

实现用 argv 调用本地 `ssh`，不经过本地 shell。SSH 会将远程命令交给远端 shell，因此配置的 agent 路径限制为绝对路径及字符 `[A-Za-z0-9_./-]`，命令参数固定；订阅值通过 stdin 发送，不拼进 shell。SSH 别名不得以 `-` 开头，只接受字母、数字、点、下划线和短横线。

消息格式为 UTF-8 JSON，一行一帧，以 LF 结束。stdout 仅有协议；stderr 为诊断，分别接收并限流。无效 JSON／登录脚本往 stdout 打印文字均作为协议错误，不猜测或跳过内容。

## 会话顺序

1. agent 启动后立即发送 `hello`，包括 `version=1`、随机 `session_id`、系统 `boot_id`、采集能力和主机元数据。
2. 客户端验证版本与 session，发送 `subscribe`。订阅消息使用递增 `request_id`。capabilities 当前作为元数据返回，尚未按能力动态裁剪面板。
3. agent 发送 `subscribed`，回显实际采用的采样间隔、进程条件和 `request_id`，随后发送快照。
4. 首次建立计数器基线时，速率／CPU 差分值为 `null`；下一次正常采样后再变成数字。
5. 切详情、排序或过滤时更新 `subscribe`。agent 发送新确认后不再输出旧订阅的快照；UI 根据 `subscription_id` 丢弃滞后的旧响应。
6. 客户端退出时关闭 stdin；agent 收到 EOF 或写 stdout 得到 EPIPE 即退出，也接受显式 `stop`。客户端回收自己创建的 SSH 子进程。

连接后 5 秒未收到 hello，或订阅后 5 秒未确认，记为握手超时。agent 建立会话后等待首次订阅的上限为 10 秒，避免留存无工作的远端进程。

双向完整样例见 [client.ndjson](examples/client.ndjson) 和 [agent.ndjson](examples/agent.ndjson)。`session_id` 绑定当前 SSH 会话，`boot_id` 返回系统启动标识；当前客户端依赖新 session 清理状态，尚未单独使用 boot_id；本地主机 ID 不从 agent 获取。

## 消息类型

| 方向／类型 | 必要内容 |
| --- | --- |
| agent → client `hello` | version、session_id、boot_id、agent_version、capabilities、host |
| client → agent `subscribe` | version、request_id、interval_ms、detail、processes、mounts、disk_devices、network_interfaces |
| agent → client `subscribed` | version、request_id、session_id、effective（完整生效订阅） |
| agent → client `snapshot` | version、session_id、subscription_id、seq、monotonic_ms、wall_time、groups |
| agent → client `error` | version、session_id、code、message、fatal、可选 request_id |
| client → agent `stop` | version、request_id |

`version` 是主协议版本。未知版本拒绝连接；v1 未知对象字段忽略，未知消息类型报告协议错误并断开。解析器拒绝非法 JSON、未知版本、重复键、超限帧／嵌套／字符串；订阅字段执行类型和范围检查。客户端还检查 session、序号和指标组基本结构，尚未实现全部指标字段的严格 schema 校验。`error.fatal=false` 表示当前请求未应用，保持上一个有效订阅；请求成功必须收到 `subscribed`。

`seq` 在当前 session 中严格递增，不因切订阅重置。客户端收到重复／回退序号时丢弃，报告协议异常；间断序号允许；当前曲线按收到的样本排列，未实现按序号插入缺口。重连必须重新订阅并清除差分基线。

## 时间与分组新鲜度

时间戳 `monotonic_ms` 是远端单调时钟毫秒值，仅在同一会话中计算时间差。`wall_time` 是 ISO 8601 UTC 字符串，只用于显示，不计算速率，也不作为跨主机严格同步依据。

各指标组统一结构：`{status, observed_monotonic_ms, interval_ms, data}`。status 取 `ok`、`warming_up`、`partial`、`unavailable`、`timeout`。采集失败时 data 为 null；无符合条件的设备时也可返回 items=[]。组内不支持或尚未预热的字段用 null 表示；磁盘／网络的 status=ok 表示设备计数读取成功，首次速率仍为 null。部分成功携带 `reason`，例如进程权限不足。缓存容量结果保留原 observed 时间，不伪装为本次新测量。

客户端维护有效快照的本地接收时刻，用其判断连接新鲜度；速率由 agent 按远端实际间隔计算。当前 UI 显示连接和分组状态，尚未实现组年龄、时钟偏移估计或精确时间轴。缓存容量结果保留原观察时间，供后续显示新鲜度。

## 指标规范

| 组 | 数据与口径 |
| --- | --- |
| cpu | usage_pct、iowait_pct、online_cores、load_avg[3]、cores[{id, usage_pct}]；全机与单核均 0–100% |
| memory | total_bytes、available_bytes、used_bytes、swap_total_bytes、swap_used_bytes；used = total − available |
| filesystems | items[{mount, total_bytes, used_bytes, available_bytes}]；used = (f_blocks − f_bfree) × f_frsize；available 用 f_bavail，预留块会使 used + available 小于 total |
| disks | items[{id, name, read_bytes_total, write_bytes_total, read_bytes_per_sec, write_bytes_per_sec}]，按块设备 major:minor 识别 |
| network | items[{ifindex, name, rx_bytes_total, tx_bytes_total, rx_bytes_per_sec, tx_bytes_per_sec}] |
| processes | total_visible、matched、returned、truncated、items[{pid, start_time_ticks, uid, name, cpu_pct, rss_bytes}] |

CPU 使用率沿用上游口径：总时间取 user、nice、system、idle、iowait、irq、softirq、steal，避免重复累计 guest；空闲包括 idle 和 iowait，另显示 iowait 占比。差分无效或总量不增加时本周期为 null，不能人为生成尖峰。

进程 cpu_pct 以一个逻辑核为 100%，多线程进程可以超过 100%；界面必须标注此口径。按 `pid + start_time_ticks` 识别进程，PID 复用不能沿用旧基线。进程列表另含 user、threads、state、ppid、nice、elapsed_seconds 和最多 192 字节的 command 摘要。选中进程详情中的 command 最多 4096 字节；不传环境变量。

网络与块设备速率在 agent 中使用实际采样间隔计算：`(counter_now - counter_previous) / elapsed_seconds`。设备消失、计数器回退、agent 重启或主机重启时重新预热。设备重现必须重新建基线；不能对未知回绕宽度做猜测。配置多块磁盘时不自动叠加磁盘和它的分区；网卡也不将隧道和底层设备混合相加。

`MemAvailable` 缺失时使用 MemFree + Cached 并以总量为上限，设置 estimated_available；这是当前简化回退值。Swap 总量为零显示 0／0，不做除零计算。每个字段的单位由名称确定，JSON 数字使用支持 uint64 的解析器，禁止中途用浮点存储累计字节数。

概览订阅 detail=false、processes.enabled=false，cpu.cores 为空数组，processes 组省略。详情设置 detail=true 并传核心明细；协议进程默认 limit=100；当前 TUI 在详情中使用 enabled=true、sort=cpu、limit=500、filter=""。filter 普通文本采用大小写不敏感的字面子串匹配进程短名称；从 `0.4.0-alpha.3` 起，`user:TEXT` 对进程的 user 字段做大小写不敏感的字面子串匹配，例如 `user:dev` 匹配 `developer`。空 `user:` 不匹配任何进程；空字符串恢复全部进程。不支持正则／shell，用户名和进程名条件不能组合。消息结构仍为 v1，旧采集器只理解进程名过滤，使用新语法必须更新目标机器的采集器。agent 在 250ms 扫描预算内读取可见进程，再过滤、按 cpu／rss／threads 降序，或 pid／name／user 升序排序（相同时 PID 升序）；reverse=true 反向排序，再取 N；切换排序后的首帧可能处于 CPU 预热状态。total_visible 是读取到的可见进程数，matched 是过滤后总数，returned 是实际返回数，truncated 表示 matched > returned；权限不足另用 partial 标记。

示例时间线：agent hello → client 第 1 行 → agent 第 2–4 行 → client 第 2 行 → agent 第 5–6 行 → client stop。两份文件分别记录两个方向，不能当作一次性发送全部请求的脚本；stop 在演示快照收完后发送。样例的第 3 个快照首次启用进程订阅，因此该组处于 warming_up。

## 断线和资源边界

| 状态 | 进入条件 | 行为 |
| --- | --- | --- |
| CONNECTING | 正在启动／认证／握手 | 保留上次结果，显示连接中 |
| ONLINE | 收到当前订阅的新快照 | 更新本机面板和趋势 |
| STALE | 超过 max(3 × interval_ms, 6000ms) 未收到新快照 | 保留旧快照并标记状态，清除 CPU 趋势 |
| DISCONNECTED | EOF、SSH 退出、读写错误，或 max(5 × interval_ms, 10000ms) 无快照 | 关闭会话，计划重连 |
| AUTH_REQUIRED | 已识别的认证／主机密钥错误 | 等待人工修复后按 r，不自动弹交互认证 |
| PROTOCOL_ERROR | 不兼容协议／帧错误 | 停止该主机重试，显示原因，按 r 重试 |

OpenSSH 的退出码不能区分所有认证和网络原因；诊断不明确时使用“SSH 连接失败”并提供受限 stderr 摘要，不冒充精确分类。网络故障采用 1、2、4、8、16、30 秒退避，附 ±20% 抖动；连续稳定收取 30 秒后清零。每台机器独立计时。

- 采样间隔允许 1000–10000 ms，默认 2000 ms；容量查询默认每 10 秒，其他组按订阅周期。
- 每帧最多 1 MiB，嵌套最多 16 层，每个普通字符串最多 4096 字节；进程过滤字符串最多 128 字节。
- 核心明细最多 2048 项，网卡最多 128，挂载／设备各最多 256，进程 limit 为 1–500。截断必须显式标记 partial／truncated；online_cores 仍为真实总数。
- 每个 agent 一次只序列化／发送一个快照，不积累采样队列；使用非阻塞 stdout，部分写入继续发送同帧，阻塞超过 10 秒退出。发送期间不产生新快照。
- UI 的 CPU 总量／分项／逐核、内存分类、磁盘、每网卡收发和进程趋势各保留最近 300 点；默认 2 秒采样约为 10 分钟，修改间隔会改变跨度。设备／进程消失后删除对应历史；选中进程使用独立的 PID + start_time_ticks 身份；精确时间窗口留待后续。
- 每台 SSH stderr 保留最多 64 KiB 尾部内容，界面显示经过处理的摘要；当前不写采样日志或完整进程信息。
- 所有远端名称在绘制前替换控制字符、ESC、换行和双向控制字符，按终端显示宽度截断，不能直接嵌入 ANSI 输出。

默认选择规则：容量查询包含 `/` 及 ext3／ext4／xfs／btrfs 挂载；块设备及分区必须有 `/sys/block` 或 `/sys/dev/block` 记录，排除 loop／ram／zram；网络包含非 loopback 接口。挂载通过 major:minor 与块设备直接匹配；无法匹配则显示 I/O unmapped。I/O 模式保留独立设备列表，不叠加整盘／分区或网卡流量。agent 支持订阅中的显式选择列表，客户端当前发送空列表。CPU 频率为可读的 CPU0 scaling_cur_freq，温度读取已识别 hwmon 类型的 temp1_input；逐核温度要求 coretemp 的 Package id／Core 标签与 CPU 拓扑匹配，均为可选字段。

只读协议不包含 signal、kill、renice 或任意命令执行请求。新客户端不将远程 PID 交给原版菜单的本机 `kill()` 路径。

## 0.3.0 的兼容扩展

主协议仍为 v1，增加可选字段。`processes` 订阅可包含 `reverse`、`selected_pid`、`selected_start`；selected_pid 为 0 表示关闭独立详情，非零必须提供 uint64 selected_start。排序还接受 pid、name、user、threads。

processes.data.selected 可为 null 或选中进程对象。成功时 detail_status=ok，返回身份、CPU／RSS、用户／状态／线程／父 PID／nice／运行时长、command、read_bytes_total、write_bytes_total。I/O 是累计字节，不是速率；读取权限不足时字段为 null。身份失效时 detail_status=gone，未扫描到或无法访问时为 unavailable，读取中途失败可为 partial；客户端不将这些状态解释为空闲。

CPU 增加 model、uptime_seconds、user_pct、system_pct、cores[].temperature_c；磁盘增加 activity_pct（忙碌毫秒差分／实际间隔 ×100，上限 100）；网络增加 address；文件系统增加 source 和 free_bytes。新增字段缺失时客户端显示 N/A。新旧版本基础 CPU／RSS 订阅仍兼容，要使用新增排序和选中详情应同步部署 0.3.0 agent。

## 0.5.0 可选资源组与 UID 过滤

新增 sessions、cards 两组，包装仍为 status / observed_monotonic_ms / interval_ms / data。
sessions 为 utmpx 去重账号与会话数、age_ms、每账号 sessions 和 first_login_at（Unix 秒）。
cards 为 [task-submit schema 1](task-submit-monitor-api.md) 的公开白名单字段。
未知或失败不能编码为零；每次转发缓存时保留原观察时间并递增 age_ms。

HTTPS 注册响应 metric_groups=["sessions","cards"] 声明扩展支持；没有声明时新 agent
回退为原 5 组。新中心仍要求原 5 组完整存在，可选组独立校验，未知组或错误计数拒收，
拒收不更新上一份有效快照。旧节点缺少新组时 TUI 显示 N/A。

filter 新增 uid:NUM，NUM 是 0..4294967295 的十进制整数，和进程 uid 做等值匹配。
空／负数／溢出 UID 不匹配任何进程；user:TEXT 的模糊匹配和普通名称过滤不变。
卡占用者联动只在新鲜卡数据且占用 UID 已知时触发，仍使用每个查看者自己的订阅。
