# 所有本机用户共享终端监控

适用于已部署 HTTPS 监控的 Linux 开发机：每台机器上的登录用户直接运行
`pto-btop`，查看原有查看凭据所授权的全部主机。中心仍只监听原 NetBird 地址；
不开放公网，也不引入浏览器界面。普通查看者不需要 sudo。

`scripts/system-install.py` 将**固定的 v0.5.0 正式包**部署为系统服务，内置两种
架构的正式包 SHA-256。它兼容 Python 3.9+，因此 Python 3.9 系统不需要先安装 uv。
当前入口不支持选择其他版本；更新版本时应使用经过验证、更新了校验和的部署入口。
已有的 `setup.py` 是个人安装入口，不能用它更新这里的系统安装。

## 安装结果

| 位置 | 用途与权限 |
| --- | --- |
| `/home/pypto-tools/pto-btop/app` | root 管理的应用，所有用户可读取、执行 |
| `/home/pypto-tools/pto-btop/config/agent` | 本机 agent 的私有配置，专用服务账号读取 |
| `/home/pypto-tools/pto-btop/config/server` | 仅中心的私有配置、服务私钥和认证凭据 |
| `/etc/pto-btop` | 所有本机用户可读的查看配置、CA 证书和只读 viewer 凭据 |
| `/usr/local/bin/pto-btop` | 自动选择公共配置的终端入口 |
| `/usr/local/libexec/pto-btop` | 原始可执行文件入口 |
| `/etc/systemd/system/pto-btop-*.service` | 系统级服务，专用非登录账号 `pto-btop` 运行 |

agent 凭据、服务私钥和 CA 私钥不会进入公共查看目录。CA 私钥继续保留在原配置
目录中。程序原有的令牌 `0600` 检查保持不变：全局入口为每次查看创建一个 `0700`
临时目录，复制 `0600` 的 viewer 令牌，退出后清理。不同用户和不同终端互不覆盖。
服务的令牌也保持 `0600`，仅专用账号和 root 可读。

查看凭据允许读取负载、进程、在线账号和卡分配情况，以及选择各自的进程筛选条件；
不授予 agent 上报权限或 task-submit 任务管理权限。

## 从已有个人安装迁移

先升级中心，再处理其他节点。在**原安装账号**的终端中执行，`$HOME` 会在 sudo
运行之前展开。`--source-root` 指向已安装目录，不是 `pto-btop-xxx-config` 配置包目录。
默认个人安装目录如下；使用过其他安装位置时相应替换。

先下载仓库中的 `scripts/system-install.py`，或在本仓库中执行它。检查模式不会
更改配置、安装程序或重启服务；只下载临时包并执行校验。

```bash
python3 system-install.py \
  --source-root "$HOME/.local/share/pto-btop" \
  --hub --proxy http://127.0.0.1:7892 --check
```

中心安装、迁移后台进程并启用开机自启：

```bash
sudo python3 system-install.py \
  --source-root "$HOME/.local/share/pto-btop" \
  --hub --proxy http://127.0.0.1:7892 --activate
```

其他节点使用同一脚本，去掉 `--hub`：

```bash
sudo python3 system-install.py \
  --source-root "$HOME/.local/share/pto-btop" \
  --proxy http://127.0.0.1:7892 --activate
```

无需重新生成、分发配置。代理仅用于 GitHub 下载，NetBird HTTPS 使用原配置。
如果不需要代理，省略 `--proxy`；如果正式包已下载，可用
`--archive /path/to/pto-btop-0.5.0-linux-arm64.tar.gz` 离线安装，仍会检查内置 SHA-256。
安装架构自动选择，`aarch64` 对应 arm64，`x86_64` 对应 amd64。

不带 `--activate` 时只安装文件，不启动服务。激活前，脚本核对旧后台进程身份；
用户级 systemd 服务则通过其原服务管理器停止并禁用。系统服务启动失败时恢复
已经停止的原后台进程或用户服务。已有系统服务重启失败时恢复原应用链接、单元
和原有运行状态。原个人安装、配置和旧程序均保留。

脚本拒绝覆盖不同的凭据、未知的全局命令或未知的同名服务。系统目标目录必须
由 root 管理，且不能允许其他用户写入。对于安装前的校验失败，不会停止原服务。

## 普通用户使用与运维

```bash
pto-btop
pto-btop --check
pto-btop --detail
```

如果原安装账号的 `~/.local/bin/pto-btop` 排在 PATH 前面，使用
`/usr/local/bin/pto-btop` 明确选择公共入口；其他未安装个人版本的用户无需处理。
`--demo`、`--local`、`--ssh`、显式 `--config`、`--help` 和 `--version` 会继续
使用对应模式，不强制使用公共配置。

```bash
systemctl status pto-btop-agent
systemctl status pto-btop-server  # 仅中心
journalctl -u pto-btop-agent -n 50 --no-pager
sudo systemctl restart pto-btop-agent
```

服务开机自启，不依赖管理员保持登录。agent 使用普通服务账号可读取的数据；
task-submit 的 `--monitor --json` 应向普通本机账号提供其公开的只读快照。
组件未安装、数据不可读或过期时，界面显示不可用，不推断空闲。

v0.5.0 保留现有容量限制：中心最多 16 个同时打开的详情视图，每台被监控主机
最多 8 个详情视图；这不是登录账号数或总览窗口数限制。

## 验证

```bash
python3 tests/cluster_system_install.py
```

常规测试覆盖公共/私有凭据隔离、进程身份校验、失败恢复、临时查看配置权限、
退出清理和显式模式。`tests/cluster_system_integration.py` 仅在隔离容器中执行：
使用模拟凭据、正式二进制、真实非 root 账号，以及模拟 systemd 管理器，验证
安装、迁移、重复运行和普通用户 HTTPS 总览/详情。它不能替代目标主机上的
systemd 激活和三机 ONLINE 验收。
