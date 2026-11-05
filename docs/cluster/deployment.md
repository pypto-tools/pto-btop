# 构建与四机接入

> 2026-11-05：0.4.0 已实现三机 HTTPS 集中接入，每机 agent 上报、中心缓存、各机 TUI 独立查看。启动和配置见 [HTTPS 使用说明](https.md)，架构见 [接入设计](centralized-https.md)。本机／SSH 模式继续保留；真实三机部署尚待确认目标和架构。

## 当前状态

已提供 `btop-agent --stdio`、`btop-cluster --local` 和重复 `--ssh ALIAS` 接入方式；本机真实采集及四路假 SSH 测试通过。以下是接入真实开发机的操作说明，尚未替用户在远端安装或修改 SSH／NetBird 配置。

构建命令和依赖见[使用说明](prototype.md)。当前基线要求 CMake ≥3.25、C++23、nlohmann/json ≥3.11；本机已验证 GCC 15.2.1、CMake 3.31.10、nlohmann/json 3.11.3。需按 x86_64／aarch64 架构和发行版验证 glibc／libstdc++ 兼容性。

## 安装布局

推荐路径：

```text
观察机
  /opt/btop-cluster/bin/btop-cluster
  /opt/btop-cluster/share/btop-cluster/themes/

每台开发机
  /opt/btop-cluster/bin/btop-agent
```

也可使用普通用户可写的绝对路径，通过 `--agent-path` 指定。当前客户端对所有 SSH 主机使用同一个 agent 路径；各机应保持一致。agent 是 SSH 按需启动的普通用户进程，无需 systemd、root 或 setuid。

将对应构建目录里的 `btop-agent` 安装到每台开发机上述路径，并验证其运行库和执行权限。观察机运行 `btop-cluster`；本机模式要求 agent 与客户端在同目录，或指定 `--agent-path`。发布打包还需包含主题、主许可证及适用的第三方声明。

## 配置与验证 SSH

主机映射交给现有 OpenSSH 配置。以下 `<...>` 为待替换占位符：

```sshconfig
Host dev-01
    HostName <dev-01-netbird-ip-or-dns>
    User <ssh-user>
    Port 22
```

其余三台分别设置 `dev-02`、`dev-03`、`dev-04`。先在普通终端核实主机身份并完成认证，再确认 BatchMode 可以无交互登录。使用已有认证方式，不在项目中保存私钥、密码或 token。

用只读的一次采样检查远端程序：

```bash
ssh -T -o BatchMode=yes -o StrictHostKeyChecking=yes \
  dev-01 /opt/btop-cluster/bin/btop-agent --once
```

预期 stdout 为一行 JSON，`groups` 包含 cpu、memory、filesystems、disks、network、processes。部分不可读指标可为 unavailable；首次容量结果也可能尚在 warming_up。shell 初始化脚本不得向非交互 stdout 输出欢迎文字。

NetBird 内网互通后，还需确保现有 SSH 服务、认证和访问策略可用。如果使用 NetBird 内置 SSH，需确认其认证支持当前的非交互连接方式；程序不会修改 NetBird 策略或绕过认证。

## 启动四机监控

```bash
./build/cluster/btop-cluster \
  --ssh dev-01 --ssh dev-02 --ssh dev-03 --ssh dev-04
```

使用其他共同安装路径时：

```bash
./build/cluster/btop-cluster \
  --ssh dev-01 --ssh dev-02 --ssh dev-03 --ssh dev-04 \
  --agent-path /absolute/shared/path/btop-agent
```

按 `1`–`4` 进入对应主机详情，`Esc` 返回总览。主题可用 `--theme themes/dracula.theme`，采样周期可用 `--interval-ms 1000`。路径必须为绝对路径且只含字母、数字、下划线、点、斜杠和短横线；主机参数使用 SSH 别名，不直接传 `user@host`。

`--config` 已用于 [HTTPS 模式](https.md)。[cluster.example.json](examples/cluster.example.json) 仍是早期 SSH 配置草案，不能作为 HTTPS 配置使用。SSH 命令行模式仍通过共同的 `--agent-path` 指定采集器；HTTPS agent 可在各自配置中指定 mounts／disk_devices／network_interfaces。

## 排障

| 界面状态 | 检查方向 |
| --- | --- |
| DISCONNECTED／握手超时 | NetBird 可达性、目标 SSH 服务、别名、端口与执行路径 |
| AUTH_REQUIRED | 在普通终端修复认证／主机密钥问题，回面板按 r |
| Cannot execute／agent not found | 程序路径、权限、CPU 架构与运行库 |
| PROTOCOL_ERROR | 两端协议版本、stdout 是否混入文本；修复后按 r |
| 单组 timeout／unavailable | 挂载响应、设备存在性和当前用户读取权限 |
| 速率 N/A | 首帧基线、设备消失／计数回退，或采集受限 |

默认周期下 6 秒无样本标记 STALE，10 秒触发重连；间隔改变时阈值相应调整。网络失败采用带抖动的 1–30 秒退避，其他三台继续更新；认证或协议错误需修复后手动重试。客户端保留受限 stderr 摘要，并在显示前处理控制字符。

## 待实机验收

完成四台真实主机的身份与指标核对、至少 30 分钟持续运行、单台断线恢复、客户端退出后的进程清理，以及 CPU／RSS／传输开销测量。性能预算见[实施计划](implementation.md)，目前这些数值是目标而非测试结论。暂不在开发机制造压力负载。
