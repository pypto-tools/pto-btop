# HTTPS 集中监控使用说明

0.4.0 系列已实现 HTTPS 上报、中心缓存和终端读取。三台机器各运行一个 agent、各自打开 TUI；中心运行在其中一台。现有 `--local`、`--ssh`、`--demo` 保留。

当前源码版本为 `0.6.0`，安装与升级见 [发布说明](release.md)。对外命令为 `pto-btop`、`pto-btop-agent`、`pto-btop-server`，下文仍使用兼容的源码构建程序名。

发行包提供 Linux amd64 / arm64 两种原生构建；CI 覆盖三个真实 agent、三个独立终端的 HTTPS 集成测试。具体节点的部署版本和连接状态请使用 `--check` 或总览确认。

0.5.0 新增登录人数和 task-submit 卡状态。先升级并重启中心，再升级各 agent / TUI；配置和凭据继续复用。详见 [资源监控](resources-monitoring.md)。

## 组件和数据

| 程序 | 运行位置 | 用法 |
| --- | --- | --- |
| `btop-server` | 中心节点 | `--config server.json` |
| `btop-agent` | 每台受监控主机 | `--config agent.json` |
| `btop-cluster` | 每台查看机器 | `--config client.json`，可加 `--detail` |

CPU、MEM、DISKS、NET 按 agent 的 `interval_ms` 持续采样，默认 2000ms。中心在内存保存最新快照及最近 300 点历史；新客户端读取基础指标的已有历史；进程历史按查看订阅累积。进程扫描只在存在查看需求时进行，每轮扫描一次，再分别计算不同查看者的排序、过滤及 PID＋启动时间详情。每份列表最多 500 项；每机最多 8 个活跃进程视图、中心最多 16 个。查看租约为 15 秒，客户端定期续期，退出后自动回收。

选择进程并按 Enter 后，会保留当前进程列表和基本信息；完整详情等待下一轮采样时，标题显示 `[loading]`，不会清空 PROC 面板。空列表不能打开详情；已退出的进程与采集不可用分别显示明确提示。

三台机器的界面操作独立。网络请求在终端的后台线程执行；慢请求不阻塞按键处理，保留差分刷新。中心的采样历史在重启后重新积累，agent 和已打开的 TUI 自动重新连接。

## 在本机试运行

以下为回环地址演示，不修改 NetBird 设置。生成目录必须不存在；工具拒绝覆盖已有密钥。示例的三台标签可以用三个本地 agent 验证连接，但它们采集的仍是同一台实际机器。

从仓库根目录执行：

```bash
python scripts/https-init.py \
  --directory /tmp/btop-https-demo \
  --listen 127.0.0.1 --server-name 127.0.0.1 \
  --hosts dev-a dev-b dev-c
```

在一个终端启动中心：

```bash
./build/cluster/btop-server \
  --config /tmp/btop-https-demo/server/server.json
```

为每个标签启动一个 agent，分别在三个终端执行：

```bash
./build/cluster/btop-agent --config /tmp/btop-https-demo/nodes/dev-a/agent.json
./build/cluster/btop-agent --config /tmp/btop-https-demo/nodes/dev-b/agent.json
./build/cluster/btop-agent --config /tmp/btop-https-demo/nodes/dev-c/agent.json
```

检查连接并打开界面：

```bash
./build/cluster/btop-cluster \
  --config /tmp/btop-https-demo/nodes/dev-a/client.json --check

./build/cluster/btop-cluster \
  --config /tmp/btop-https-demo/nodes/dev-a/client.json --detail
```

其他终端可使用 dev-b／dev-c 的 `client.json`。`--check` 不进入全屏，输出每个授权主机的状态和 CPU 摘要；连接未就绪时返回非零。`--once --detail` 等待有效基础样本及请求的进程视图后输出完整 ANSI 帧；整体等待有上限。

## 三台真实机器的接入

1. 确认三台的 SSH 或其他部署入口、Linux 架构和运行库；为各机准备可运行的 agent／TUI。运行期使用 HTTPS，部署方式不限定 SSH。
2. 在中心生成正式配置，`--listen` 填本机 NetBird IP，`--server-name` 填所有节点实际用来访问中心的 NetBird DNS 名称或 IP。端口默认 8443，可用 `--port` 修改。
3. 每台只分发 `nodes/<本机ID>/` 目录。中心保留原始配置树，供服务端读取每机凭据；CA 私钥和服务端私钥留在中心。只读查看凭据与采集凭据是不同文件。
4. 配置 NetBird 策略允许这三台连接中心的 TCP 8443。B、C 上的 agent 主动上报，无需新增入站监听端口；本次没有自动修改访问策略。
5. 启动中心和三个 agent，分别执行 `--check`，然后在每台打开 TUI。

生成示例中的 `SERVER_NETBIRD_IP` 和 `SERVER_NETBIRD_NAME` 必须替换为真实值：

```bash
python scripts/https-init.py \
  --directory "$HOME/.config/btop-cluster" \
  --listen SERVER_NETBIRD_IP \
  --server-name SERVER_NETBIRD_NAME \
  --hosts dev-a dev-b dev-c
```

工具生成包含正确 SAN 的服务端证书，默认有效期一年；私有 CA 有效期十年。到期前需重新签发证书并安排重启服务，当前未实现自动续期。agent／client 始终验证 CA 和名称，没有关闭证书检查的选项。凭据文件要求至少 32 个 URL-safe 字符、常规文件、仅所属用户可读写；工具创建为 0600。真实配置不放入仓库。

## 配置格式

路径相对于对应 JSON 文件解析。未知配置字段报错，避免把拼错的设置静默忽略。旧的 `examples/cluster.example.json` 是 SSH 配置草案，不适用于新的 HTTPS `--config`。

客户端：

```json
{
  "version": 1,
  "server": "https://monitor.example:8443",
  "ca_file": "ca.pem",
  "token_file": "viewer.token"
}
```

agent 使用相同连接字段，`token_file` 指向它自己的 `agent.token`；可增加 `interval_ms`（1000～10000）、`mounts`、`disk_devices`、`network_interfaces`。空选择列表沿用现有自动选择规则。采样周期由 agent 决定，TUI 的 `--interval-ms` 不改变 HTTPS agent 的周期。

服务端由生成工具输出：包含 `listen`、`port`、`cert_file`、`key_file`、`hosts` 和 `viewers`。每个 host 有稳定 `id`、可选 `label`、独立 `token_file`；每个 viewer 有自己的 `id`、`token_file` 和允许查看的 `hosts` 列表。支持 1～4 台主机、最多 16 个查看凭据；绑定地址必须是明确 IP，不能使用通配监听地址。

凭据、证书和主机配置在启动时读取。修改后重启对应组件；服务端重启会清空内存历史和查看租约。

## 服务启动与安装

现有构建增加 OpenSSL ≥3.0 开发库依赖，使用固定的 cpp-httplib v0.64.0，源码、MIT 许可证和 SHA-256 记录位于 `include/httplib/`。配置和构建命令仍见 [原有使用说明](prototype.md#构建)。

构建完成后，可以只安装监控组件：

```bash
.tools/cmake-3.31.10-linux-aarch64/bin/cmake \
  --install build/cluster --component btop-cluster --prefix "$HOME/.local"
```

安装包含三个程序、主题和适用的许可证，不替换系统 btop。此命令在其他架构使用当地可运行的 CMake。当前为动态链接产物，仍需对应环境的 glibc／libstdc++／OpenSSL。

普通用户 systemd 服务模板位于 `scripts/systemd/`：

- `btop-agent.service` 使用 `%h/.config/btop-cluster/node/agent.json`。
- `btop-server.service` 使用 `%h/.config/btop-cluster/server/server.json`。

每机把它自己的 node 配置目录放到上述路径；中心可以将 `nodes/<本机ID>` 复制为 `node`。将所需模板放入 `~/.config/systemd/user/` 后执行：

```bash
systemctl --user daemon-reload
systemctl --user enable --now btop-agent.service
# 仅中心节点：
systemctl --user enable --now btop-server.service
```

也可以按实际安装位置修改模板的 `ExecStart`。用户服务是否在退出登录后继续运行取决于该机器的用户服务／linger 配置；本次没有创建系统用户、开启 linger 或安装启动服务。

## 验证与边界

新增验证覆盖真实 TLS 的 CA／主机名检查、角色与主机访问权限、会话与递增序号、非法快照不推进缓存、单轮扫描的独立进程查询、三个 agent 与三个 PTY、查看租约过期、单机离线、中心重启恢复，以及故障期间终端退出和终端属性恢复。原有采集／SSH／布局／防闪烁测试继续保留。

JSON 消息及响应限制为 8 MiB，配置文件为 64 KiB；进程扫描仍有 250ms 预算。HTTP 工作线程、任务队列、查看数量和历史点数有界。`429` 表示查看数量达到上限；`413` 表示消息或历史响应超过限制。缺失权限或超出采集预算的指标显示 unavailable／partial，不以零代替。

默认 2 秒采样时，6 秒无数据标记 STALE，10 秒标记 DISCONNECTED，阈值随采样周期调整。保留最后快照及时间；恢复时清理断档趋势，不填造中间采样。没有高可用、历史落盘、长时间补传或自动证书续期。真实三机长期运行、不同架构安装包以及实际 CPU／RSS／流量开销仍待实机验收。
