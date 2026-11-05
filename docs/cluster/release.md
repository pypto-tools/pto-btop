# 安装、升级与发布

项目对外名称为 `pto-btop`，本地源码目录仍可保留 `ci-tools/btop-cluster`。
独立仓库为 [pypto-tools/pto-btop](https://github.com/pypto-tools/pto-btop)，
首个预发布版本准备使用 `v0.4.0-alpha.1`。构建结果见 [GitHub Actions](https://github.com/pypto-tools/pto-btop/actions/workflows/release.yml)，
已发布安装包以 [Releases](https://github.com/pypto-tools/pto-btop/releases) 为准。

## 包与兼容性

每个架构一个包，包含终端、agent、server，以及主题、许可证、安装脚本和
配置生成器：`pto-btop-VERSION-linux-{amd64,arm64}.tar.gz`，附 SHA-256 文件。
Linux `x86_64` 使用 amd64，`aarch64` 使用 arm64。

使用 glibc 和 OpenSSL 3；安装与配置生成需要 Python 3.10+，生成证书另需
`openssl` 命令。包中的 `package.json` 记录实际要求的 glibc 版本和动态库。
`install.py --check` 校验文件、CPU 架构、glibc，并启动三个程序的 `--version`
确认动态库可加载。校验和用于验证下载完整性，必须从可信发布渠道同时获取包
和校验文件；不要将包内清单当作独立签名。

发布构建使用 `BTOP_CLUSTER_STATIC_CXX_RUNTIME=ON`，随程序静态链接 GCC 的
C++ 运行库；OpenSSL 和 glibc 保持系统动态链接。不是完全静态或 Alpine 包。
包内保留 GPLv3 文本和 GCC Runtime Library Exception，以及其他依赖许可证。

## 安装与更新

先检查校验和、解压，在包目录执行：

```bash
python3 install.py --check
python3 install.py --root "$HOME/.local/share/pto-btop" --bin-dir "$HOME/.local/bin"
"$HOME/.local/bin/pto-btop" --local --detail
```

安装参数默认是组织约定的 `/home/pypto-tools/pto-btop` 和 `/usr/local/bin`。
上例选择用户有权限的位置；集中安装时由管理员先给运行账号配置目录写权限，
或按实际权限执行安装。不要在 root 生成凭据后假定普通用户可以读取它们。

安装布局：

```text
pto-btop/
  app/releases/<version>-<arch>-<digest>/  # 原始、已校验的应用包
  app/current -> releases/...
  app/systemd/                           # 针对安装路径生成的用户服务模板
  config/                                # 人工生成或分发的配置
  state/
  logs/
  tmp/
```

命令目录只新增 `pto-btop`、`pto-btop-agent`、`pto-btop-server` 三个入口，指向
`app/current/bin/`，拒绝覆盖其他同名命令。程序之间可继续使用原有 `btop-*` 名称。
安装不启动服务、不改 NetBird、不生成密钥。新建的配置和状态目录仅所属账号可读写；
已有目录权限与内容保持不变。

更新时解压新包，使用相同 `--root` 和 `--bin-dir` 再运行安装脚本。验证通过后
原子切换 `app/current`，旧版保留在 `app/releases/`。已运行的进程继续使用旧程序，
需要显式重启已有 agent/server 服务并重新打开 TUI 才会使用新版。
回退可运行旧版目录内的 `install.py`，使用相同安装参数，然后显式重启服务。
配置格式变化时先阅读该版说明；应用回退不会回滚配置。

## 配置与服务

安装后从 `app/current/scripts/https-init.py` 生成配置，目标可用工具根目录的
`config/enrollment`（必须不存在）：

```bash
PTO_BTOP_ROOT="$HOME/.local/share/pto-btop"
python3 "$PTO_BTOP_ROOT/app/current/scripts/https-init.py" \
  --directory "$PTO_BTOP_ROOT/config/enrollment" \
  --listen SERVER_NETBIRD_IP --server-name SERVER_NETBIRD_NAME \
  --hosts dev-a dev-b dev-c
```

将占位地址替换为真实 IP/名称。中心保留 enrollment 完整目录；每台只接收
`enrollment/nodes/<id>`，放到自己的 `config/node/`。
中心服务模板默认读取 `config/enrollment/server/server.json`。
配置文件中的相对路径按对应 JSON 文件的目录解析，详见 [HTTPS 说明](https.md)。

先通过前台命令验证联通；需常驻时，显式复制所需模板并启用用户服务：

```bash
mkdir -p "$HOME/.config/systemd/user"
cp "$PTO_BTOP_ROOT/app/systemd/pto-btop-agent.service" "$HOME/.config/systemd/user/"
systemctl --user daemon-reload
systemctl --user enable --now pto-btop-agent.service
```

仅中心节点另安装并启用 `pto-btop-server.service`。如不采用上述配置布局，先修改
模板的 `--config`。用户登出后是否持续运行取决于该机器的 user manager/linger
配置；安装器不会修改它。指标按运行账号的读取权限显示。

## 构建与发布流程

`VERSION` 是唯一程序版本源，CMake 生成头文件供三个程序和 agent hello 使用。
本地构建见根 README。打包脚本从 CMake 的 `btop-cluster` 组件安装，检查实际
ELF 依赖和版本；不重新编译，不打包配置、测试密钥、原版 btop 程序或本地参考截图。
输出目录已有同名资产时拒绝覆盖。

CI 使用原生 `ubuntu-24.04` 和 `ubuntu-24.04-arm` runner，在同一 GCC 14 / Debian 12
容器中构建测试，限制发布包的 glibc 要求不超过 2.36。镜像固定为：

```text
gcc@sha256:bf543463a2a7171654faed7d899d74a286fd3706ffb424f52a2cb4dc3f696039
```

摘要取自 [Docker 官方镜像历史记录](https://github.com/docker-library/repo-info/blob/feb0bc4bc9fcdfb0a9e348d01794bc8b537f48ba/repos/gcc/remote/14-bookworm.md)。
原来的 `14-bookworm` 可变标签已撤下，因此按摘要引用。远程实际拉取和双架构执行
仍需首轮 CI 验证；本机网络不能访问 Docker Registry。本地包使用现有 GCC 15 和
glibc 2.36 构建，不应称为已经验证了该容器。apt 构建依赖仍使用 Debian 仓库更新；
当前不承诺不同日期生成的包逐字节一致。

1. 整理并提交变更，在 `pypto-tools/pto-btop` 建立独立仓库，保留上游来源与许可证。
2. 推送 `main`，确认两个架构都通过采集、HTTPS、终端及安装升级测试。
3. 确认 `VERSION` 和 `docs/cluster/releases/vVERSION.md` 一致，再推送版本标签。
4. 标签触发相同的构建、测试和打包。两个任务成功后，自动创建带两个安装包、
   校验文件和说明的 **draft prerelease**。重复标签不会覆盖已有 Release。
5. 检查草稿和实际机器验收结果，再明确发布预览版。第一版应标为预发布。

本机参考截图在 `.gitignore` 中保留，仅使用模拟数据生成的 `detail-preview.png`
对外展示。上游原有 README、贡献规范和自动化配置归档于 `docs/upstream/`，
避免在独立仓库误触发原来的 Snap、macOS/BSD 或上游发布流程。
