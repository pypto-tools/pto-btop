# task-submit 监控查询接口约定

状态：pto-btop 0.5.0 已接入 schema_version 1。
本地查询只转发下列公开字段；daemon 身份验证和快照发布由 task-submit 负责。
相关界面：[登录与卡资源设计](resources-monitoring.md)。

## 一个本地只读入口即可

```bash
task-submit --monitor --json
# 同一入口也可由现有别名提供：
pto-task --monitor --json
```

monitor 需要普通监控账号读取**整机、所有用户**的分配摘要，不需要 sudo。
建议 daemon 原子写出仅含下面字段的 `state/monitor.json`，CLI 只读取快照、计算年龄、
输出单个 JSON 对象后退出。快照文件可以通过普通只读权限或专用只读组暴露。
不能让监控进程读取 `.env`、密码、完整命令行、工作目录或执行任何任务。

命令不受调用者 cwd 的 `task-submit.conf`、TASKQUEUE_DEVICE_POOL 等项目策略影响。
全局池、拓扑、维护和预留来自正在工作的 daemon；不是重新在监控账号环境里模拟
一次任务提交。保留现有 `--list` / `--devices status` 的人类输出，不强行解析它们。

## 必需的数据

| 字段 | 含义 |
| --- | --- |
| `schema_version` | 整数 1；结构变化有明确版本 |
| `producer` | 组件名、实际运行 daemon 的版本／revision |
| `boot_id`, `snapshot_seq` | 快照所属启动身份及递增序号；重启后不得把旧快照当新数据 |
| `observed_at`, `age_ms` | 原始观察时间 UTC、距观察的实际年龄；读取缓存不能更新观察时间 |
| `inventory_complete` | 是否已完整枚举硬件；false 时 summary 的所有计数必须为 null |
| `status`, `reason` | `ok` / `partial` / `unavailable`；缺字段、权限不足或未完成枚举不能假装成功 |
| `scheduler.state` | `running` / `maintenance` / `stopped` / `unknown` |
| `scheduler.mode` | 当前策略，例如 `backfill` / `pool_aware_reservation` |
| `scheduler.accepting_submissions`, `scheduler.dispatching` | 是否接受提交、当前是否允许调度；均可为 null 表示未知 |
| `scheduler.running_tasks`, `pending_tasks`, `max_concurrent` | 全机任务数量和并发上限；CPU-only 任务也计入，不能按持卡数推导任务数 |
| `scheduler.reason` | 维护／阻塞等可展示原因，正常时 null |
| `devices[]` | 完整真实设备 ID 列表；不能假设 0..N-1；最多 256 项 |
| `devices[].id`, `group_id` | 与 `--device` 一致的逻辑卡号（字符串）、HCCS 组；未知 group_id 为 null |
| `devices[].pool_eligible` | 是否在 daemon 当前全局自动池内，未知为 null |
| `devices[].state` | `free` / `allocated` / `reserved` / `disabled` / `external` / `error` / `unknown` |
| `devices[].reason_codes` | 不可用原因数组；例如 `allocated`, `reservation_pool`, `outside_auto_pool` |
| `devices[].allocation` | 当前占用者对象或 null；字段见下文 |
| `devices[].reservation` | 当前预留／受保护池对应任务对象或 null；可以与 allocation 同时存在 |

占用对象包含 `task_id`、`user`、`uid`、`started_at`、`elapsed_seconds`。
`user` 是 SUBMIT_USER，不是 root daemon 或运行身份。无法解析字段明确 null；
有锁但无法确定持有人时仍不能显示为 free。预留对象包含 `task_id`、`user`、`uid`、
`requested_devices`（请求卡数），它表示等待分配，不能冒充当前占用者。

必须附带 `summary`：total、free、allocated、reserved、disabled、external、error、
unknown、occupying_users。前 7 种设备状态计数之和等于 total；occupying_users 按
占用任务提交者的 UID 去重，不含仅等待预留的用户。无法得到完整数量时返回 null
并标记 partial / unavailable；不能将截断列表当全量。

`summary` 不是分配判断来源；以设备明细和 scheduler 状态为准，计数不一致视为
损坏快照。完整示例：[task-submit-monitor.example.json](examples/task-submit-monitor.example.json)。

## free 与不可用的准确语义

- free：全局自动池内，当前没有实际分配/持锁，也不在为先到任务保护的池内。
- allocated：已经分配给任务，包括启动/清理中的占用；有预留池叠加时仍显示 allocated，
  reservation 另存等待者。多卡任务在各设备引用同一 task_id，不创建假任务。
- reserved：当前没有占用，但被策略保护，不能借给后来的普通任务。
- disabled：全局池排除或管理员明确禁用；与错误/维护的具体原因分开。
- external：只有在已验证存在队列外占用时使用，不能凭推测给用户或任务归属。
- error：已确认故障；unknown：无法可靠判断分配/持锁状态。

已部署接口优先处理 unknown，再处理 allocated、disabled、reserved、free；
pto-btop 保留原始 state，不根据颜色或池成员关系自行重算。
原始 pool_eligible / allocation / reservation / reason_codes 完整保留。
遇到证据不完整时可以确定“被占用”，但不能确定“空闲”；不得仅凭一个旧 PID 存活或
一个锁文件是否存在判断。应结合 daemon 的分配状态、有效锁与进程身份；PID 身份需
考虑复用。查询本身不能尝试抢占设备锁、重排队列或释放设备。

free 描述队列资源，不承诺新提交会立即启动：全机并发上限、维护模式、已有排队、
项目白名单和同组要求仍生效。monitor 显示这些限制，不能在客户端重做调度决策。

预留池必须由 daemon 在调度时导出。例如当前任务占卡 5，而等待任务保护 [5,6]：
卡 5 的 allocation 和 reservation 都有值；卡 6 状态为 reserved，不计入 free。
这是现在的文本查询和锁扫描最容易漏掉的信息。

## 硬件指标是可选扩展

当前必需 `health`（无检测时为 unknown）和 `external_detection`（当前为 unsupported）。
未来可选字段：`name` / `model`、`utilization_pct`、`memory_used_bytes`、
`memory_total_bytes`、`temperature_c`、硬件观察时间、物理卡/NPU/chip 映射。
没有这些指标也可以完成第一版“谁占了卡、哪些卡可用”的队列视图。

不要把 0% 利用率等同于空闲，也不要把 task-submit 的逻辑卡号用固定除法映射为
npu-smi 的卡/chip。若暂不检查硬件健康或队列外进程，输出 `health: "unknown"`、
`external_detection: "unsupported"`；界面注明仅反映队列。不要伪造全部 healthy。

## 一致性、时效和失败行为

- 同一次快照里的设备、计数、队列和预留必须一致。daemon 以临时文件 + 原子 rename
  发布，监控不观察写到一半的 JSON，不从多次独立查询拼一个“同时刻”。
- 推荐至少每 2–5 秒更新一次，变更时及时发布；age_ms 使用同一机器的单调时钟计算。
  boot_id / daemon 身份变化要使旧快照失效。只有 PID 文件存在不足以证明 daemon 活着。
- stdout 只输出 UTF-8 JSON；诊断去 stderr，无 ANSI、进度条或混杂帮助文本。
- 读取正常快照退出 0（stopped / maintenance 也是可描述的状态）；不能读取、JSON
  损坏、版本不支持退出非 0。旧缓存若返回，必须保留真实 age_ms 和失败/停止状态。
- 目标延迟 <=500ms；监控会在 1 秒后终止本次查询，不影响队列。查询不要同步遍历
  历史任务、调用每张卡的慢 npu-smi、下载内容、提权或等待交互。
- stdout 必须 <=256KiB，设备最多 256 项；null 与 0 区分。
- age_ms 超过 15000、cached=true 或观察年龄未知，界面不宣称空闲。
- CLI 非零退出时仅接受 status=unavailable 的有效 JSON 作为失败诊断，不能将其标成新鲜。

## task-submit 侧验收清单

- 普通账号可读所有用户的脱敏卡摘要；不会泄露环境变量或完整命令。
- 无任务、无卡、非连续卡号、同一用户多任务、多卡任务。
- 分配→启动→运行→清理→释放全流程没有把实际占用显示成 free 的中间窗口。
- 预留、占用与预留重叠、取消预留任务、全局池更新、维护和并发上限。
- daemon 停止/重启、旧缓存、文件原子替换、权限错误、PID 复用或持有人未知。
- 从不同目录、不同普通账号、带有项目设备环境变量调用，返回同一全机资源口径。
