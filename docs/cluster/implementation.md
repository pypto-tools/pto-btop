# 实施计划与验收标准

> 2026-11-05：0.4.0 已实现三机 HTTPS 集中接入，每机 agent 上报、中心缓存、各机 TUI 独立查看。启动和配置见 [HTTPS 使用说明](https.md)，架构见 [接入设计](centralized-https.md)。本机／SSH 模式继续保留；真实三机部署尚待确认目标和架构。

## 阶段与依赖

当前已完成 P0 和 P1，P2 的本机／四路 SSH、重连隔离及 P3 的五区域详情也已实现；HTTPS 配置、中心服务、多查看者进程查询和后台读取已完成；SSH 配置草案、完整时间轴和更细显示仍待补齐。24 项 CTest 通过，实际命令见[使用说明](prototype.md)。下列实机和性能验收尚未完成。

| 阶段 | 当前状态 | 后续工作 |
| --- | --- | --- |
| P0 基线与拆分 | 原版构建、绘图共享、无终端 agent 均完成 | 保持原版回归 |
| P1 协议与采集 | v1 NDJSON、CPU／内存／容量／块设备／网络／按需进程完成 | 扩展异常协议与慢源故障注入 |
| P2 四机连接 | 本机管道、最多四个 SSH、超时／退避、独立快照完成 | SSH JSON 配置文件、真实网络断线验收（HTTPS 配置和每机选择已实现） |
| P3 总览与详情 | 四机总览、原版风格布局、分类历史、磁盘映射、网络缩放、进程树／选择／详情完成 | 时间对齐与数据年龄、更多原版详情能力 |
| P4 实机交付 | HTTPS 本机端到端验证、配置生成和用户服务模板完成 | 四台真实开发机安装、持续运行、性能与 ABI 验收 |

## 当前文件与依赖

| 文件 | 已实现职责 |
| --- | --- |
| `CMakeLists.txt`、`src/cluster/CMakeLists.txt` | 保留 btop，增加 BTOP_CLUSTER；新 agent／客户端及独立库；find_package 获取 nlohmann/json ≥3.11 |
| `src/btop_draw_primitives.cpp`、`src/btop_draw.hpp` | 提取 Graph、Meter、createBox 和显式 Style；兼容原版入口 |
| `src/btop_render_support.cpp`、`src/btop_theme_legacy.cpp` | 原版终端常量／字符串辅助与主题兼容入口 |
| `src/collector/linux.*`、`filesystems.*` | 实例化 Linux 采集器、差分计数、独立容量查询工作进程 |
| `src/protocol/protocol.*` | v1 编解码、流式分帧、重复键／大小／深度限制、订阅选项与限时输出 |
| `src/agent/main.cpp` | headless 生命周期、hello、订阅、周期采样、一次采样 |
| `src/cluster/main.cpp`、`terminal.*` | CLI、输入、独立终端生命周期、过滤编辑 |
| `src/cluster/session.*` | 本机或 SSH 子进程、非阻塞管道、订阅、状态、重试与回收 |
| `src/cluster/view.*`、`detail.cpp` | 每主机快照和历史、总览、cpu／mem／disks／net／proc 详情 |
| `tests/cluster_collection.cpp`、`cluster_render.cpp` | 计数口径、PID 重用、协议、布局、名称安全显示 |
| `tests/cluster_live.py`、`cluster_terminal.py`、`terminal_support.py` | 实际 agent、四路假 SSH、一路故障、PTY 交互与终端恢复 |

collector 和 protocol 不链接终端、主题、Runner 或 Config；客户端不链接 collector。Linux collector 按原版 CPU／内存口径实现独立读取和差分状态，原版 `src/linux/btop_collect.cpp` 仍保留。未把整个 libbtop 链接进 agent，也未把原版所有面板直接复制成四套全局状态。

下面保留设计阶段的接口示意及验收目标，实际类型／文件名以本节和源码为准；尚未实现的 legacy collector adapter、时间窗口和后台队列不视为交付能力。

## 改造前后示例

当前 `src/btop.cpp:538` 附近的运行模型可概括为：

```cpp
auto cpu = Cpu::collect(conf.no_update);
// The existing renderer also reads process-global UI and host state.
output += Cpu::draw(cpu, /* GPU and redraw options */);
```

新客户端只消费已收到的远端数据，示意接口如下（省略 include 和辅助类型）：

```cpp
// Proposed: src/cluster/view.hpp
struct HostViewState {
    Rect bounds;
    Draw::Graph cpu_graph;
    Draw::Meter memory_meter;
    std::string selected_interface;
};

std::string draw_host_card(const HostSnapshot& snapshot,
                          const HostHistory& history,
                          HostViewState& view,
                          const ThemeContext& theme);

// Proposed: the UI loop; no local collector calls here.
for (const auto& host : host_store.hosts()) {
    output += draw_host_card(host.snapshot, host.history,
                             views.at(host.id), theme);
}
```

每张卡片自己的 `Draw::Graph` 接收对应主机的历史；原版 `Cpu::draw` 中静态／全局缓存不参与多机绘图。快照缺失时用单独的 connecting／stale 绘制路径。

当前 `src/linux/btop_collect.cpp:1185` 将采集历史与面板宽度关联：

```cpp
while (cmp_greater(cpu.cpu_percent.at("total").size(), width * 2))
    cpu.cpu_percent.at("total").pop_front();
```

新 collector 只保存差分计算需要的前值。以下是接口草案，不是已编译的实现：

```cpp
// Proposed: src/collector/linux.hpp
class LinuxCollector {
public:
    Result<Snapshot> sample(const CollectionOptions& options,
                            MonotonicTime now);
private:
    CounterBaseline previous_;
};

// Proposed: client-owned history with timestamped missing values.
history.push(snapshot.sample_time, snapshot.cpu.usage_pct);
history.trim_before(now - std::chrono::minutes(10));
history.limit_points(600);
```

原版 btop 通过 legacy adapter 将 collector 结果继续写入其现有 deque、保留 width 截断行为；新 agent 将同一个结果转换成协议。提取中如果暂时保留少量上游全局状态，它只能存在于 agent 的单机进程中，必须列出剩余依赖并在 P1 出口消除 UI 依赖。

## 构建设计

新增目标遵循当前上游 C++23，首版 Linux 禁用 GPU。新的目标关系：

```text
btop-cluster ── cluster UI ── draw primitives / terminal / theme
             └─ host store / SSH ── protocol
btop-agent   ── agent loop ── collector + protocol
btop         ── legacy adapters + existing UI + extracted components
```

构建产物使用独立名称和安装目录，避免替换系统原有 btop。CMake 选项 `BTOP_CLUSTER=ON` 已实现，用于构建客户端和 agent；`BTOP_GPU=OFF` 用于已验证的 CPU-only 基线。部署命令和工具链条件见[部署方案](deployment.md)。

## 测试与验收

| 测试类别 | 必须覆盖的场景 |
| --- | --- |
| 协议 | 一帧拆成多次读、多帧一次读、超长帧、截断 JSON、重复键、未知版本、uint64 大数、非有限数、错误类型和非法控制字符 |
| 状态机 | session／boot 改变、重复序号、旧订阅迟到、无帧超时、退避上限、认证提示、进程退出和 stderr 填满 |
| 采集 | 第一次预热、间隔不是整秒、CPU 核数变化、PID 重用、无 Swap、MemAvailable 缺失、网卡消失、计数器回退、权限不足 |
| 隔离 | dev-01 满载而 dev-02 空闲；反复切换后两台曲线、网卡、CPU 核数和进程列表仍保持独立 |
| 慢源 | 一个容量查询卡住；CPU／内存照常输出；不无限创建工作进程，恢复后重新标记新鲜度 |
| 绘图 | 120×36、80×24、低于最小尺寸、超长 UTF-8 名称、宽字符、256 色／TTY 模式、主题切换、断线缺口 |
| 集成 | 4 个假 SSH 端点并行；只断一个；订阅切换；按 q、SIGINT、SIGTERM 后恢复光标和终端模式 |
| 回归 | 原版 btop 单机启动、主题、CPU／内存／网络／进程面板与现有 GTest 测试 |

集成先使用本地假 agent 和 PTY，避免依赖 4 台真机才能推进。真实 CPU、网络吞吐及磁盘负载实验必须在确认的测试窗口执行；设计阶段不在开发机制造压力。

实机验收标准：

1. 4 台机器连续显示至少 30 分钟；一台断开后其余三台仍按 2 秒周期更新。
2. 默认周期下，6 秒未收到样本显示 stale，10 秒触发重连；模拟恢复后自动回到 online。
3. 所有状态显示主机名和数据年龄；旧读数不冒充当前数据，曲线不把断线填零。
4. 主机、网卡和进程排序切换正确；4 台配置中的别名映射到预期机器。
5. 关闭客户端后正常情况下 5 秒内回收本地 SSH 和远端 agent；网络黑洞时 agent 最迟在 stdout 阻塞超时或 SSH 断开后退出。内核不可中断 I/O 另行记录，不伪称可强制即时回收。
6. 性能预算作为待测目标：在 64 逻辑核、进程订阅关闭时，单 agent 平均 CPU <1% 单核、RSS <50 MiB、单机传输 <50 KiB/s；四机客户端平均 CPU <5% 单核、RSS <150 MiB。详情／大核数单独记录，超预算先定位采样或绘图来源。
7. 窗口放缩和按键响应目标 p95 <100ms；CPU 比较允许采样窗口差异，不以某次 top 数值逐字相等为判据。

测试输出需要记录源码提交、构建工具链、主机核数、进程数量、采样周期和是否启用详情。以上性能数值是验收目标，不是当前测量结果。
