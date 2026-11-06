# 安装、升级与发布

项目对外名称为 `pto-btop`，本地源码目录仍可保留 `ci-tools/btop-cluster`。
独立仓库为 [pypto-tools/pto-btop](https://github.com/pypto-tools/pto-btop)，
当前正式版本为 `v0.5.0`。构建结果见 [GitHub Actions](https://github.com/pypto-tools/pto-btop/actions/workflows/release.yml)，
已发布安装包以 [Releases](https://github.com/pypto-tools/pto-btop/releases) 为准。

## 包与兼容性

每个架构一个包，包含终端、agent、server，以及主题、许可证、安装脚本和
配置生成器：`pto-btop-VERSION-linux-{amd64,arm64}.tar.gz`，附 SHA-256 文件。
Linux `x86_64` 使用 amd64，`aarch64` 使用 arm64。

发布包支持 glibc 2.28+，随程序静态链接 OpenSSL 3.5.9，无需系统安装 OpenSSL 3。
安装与配置生成需要 Python 3.10+，可通过 uv 提供；生成证书另需
`openssl` 命令（支持 1.1.1 和 3.x，使用独立配置生成证书扩展）。包中的 `package.json` 记录实际要求的 glibc 版本和动态库。
`install.py --check` 校验文件、CPU 架构、glibc，并启动三个程序的 `--version`
确认动态库可加载。校验和用于验证下载完整性，必须从可信发布渠道同时获取包
和校验文件；不要将包内清单当作独立签名。

发布构建使用 `BTOP_CLUSTER_STATIC_CXX_RUNTIME=ON` 和
`BTOP_CLUSTER_STATIC_OPENSSL=ON`，静态链接 GCC C++ 运行库和 OpenSSL；
glibc 保持系统动态链接。不是完全静态或 Alpine 包。
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

## 已有节点配置时直接安装

保留原配置目录里的 `node.json` 和 `node/`。在该目录下载新版安装入口；
入口本身不含任何节点地址或凭据，两个架构的包校验和在发布时固定写入。
更新程序无需重新分发或生成配置。以下示例代理只用于下载：

```bash
curl --proxy http://127.0.0.1:7892 --noproxy '' -fL \
  -o setup.py https://github.com/pypto-tools/pto-btop/releases/download/v0.5.0/setup.py
curl --proxy http://127.0.0.1:7892 --noproxy '' -fL \
  -o setup.py.sha256 https://github.com/pypto-tools/pto-btop/releases/download/v0.5.0/setup.py.sha256
sha256sum --check setup.py.sha256
HTTPS_PROXY=http://127.0.0.1:7892 \
  uv run --no-project --python 3.12 setup.py --proxy http://127.0.0.1:7892
```

uv 自动获取所需 Python；服务器已提供 Python 3.10+ 时可以直接运行
`python3 setup.py --proxy http://127.0.0.1:7892`。不需要 sudo。
uv 的安装说明见 [官方文档](https://docs.astral.sh/uv/getting-started/installation/)。
`setup.py --check` **仅检查，不安装、不复制配置、不启动采集器**。
`--install-only` 安装但不启动；无这两个参数时安装并启动采集器。
脚本校验包、NetBird 地址和已有配置，拒绝覆盖不同的节点配置。
默认安装至 `~/.local/share/pto-btop`，命令位于 `~/.local/bin`。

安装成功后查看所有已授权节点：

```bash
"$HOME/.local/bin/pto-btop" \
  --config "$HOME/.local/share/pto-btop/config/node/client.json" --detail
```

已有服务升级后仍需显式重启才能载入新程序；配置和日志保留。新安装优先使用
当前账号可用的 user systemd，否则在后台启动；开机自启取决于服务管理器配置。

更新已有节点以启用新采集能力时，在原配置目录下载新版 `setup.py` 后运行：

```bash
HTTPS_PROXY=http://127.0.0.1:7892 \
  uv run --no-project --python 3.12 setup.py \
  --proxy http://127.0.0.1:7892 --restart-agent
```

`--restart-agent` 仅重启该节点的采集器。用户 systemd 服务通过服务管理器重启；
后台进程须匹配记录的启动命令，并使用 Linux pidfd 将停止信号绑定到正确的进程，
拒绝停止复用该 PID 的其他命令。不支持 pidfd 的系统需自行通过服务管理器停止
采集器后再运行安装入口。该参数不能与 `--check` 或 `--install-only` 同用。

需要让本机所有登录用户直接使用 `pto-btop` 时，使用
[系统服务部署入口](system-service.md)，保留原有配置并迁移到专用服务账号。

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

CI 使用原生 `ubuntu-24.04` 和 `ubuntu-24.04-arm` runner，在 PyPA 的
manylinux_2_28（AlmaLinux 8、GCC 14）容器中构建测试，限制发布包的 glibc
要求不超过 2.28。镜像摘要于 2026-11-05 从官方 Quay API 解析：

```text
quay.io/pypa/manylinux_2_28_x86_64@sha256:7e41b1c7fd64dfc69703a1124b43c404382973ee06ec5d764b85c34d2221fded
quay.io/pypa/manylinux_2_28_aarch64@sha256:54b271d9562a3686298880df05367909e949ed317e7b1b79b59c696d75f78676
```

来源与基线见 [PyPA manylinux](https://github.com/pypa/manylinux)。容器补充 Perl
构建模块，`scripts/build-release-deps.sh` 校验固定 SHA-256 后构建 OpenSSL
3.5.9 静态库和 nlohmann/json 3.11.3。CMake 的 `OPENSSL_ROOT_DIR` 和
`CMAKE_PREFIX_PATH` 指向依赖安装前缀，禁止发布包动态依赖 libssl/libcrypto。
CI 在同一 glibc 2.28 环境运行真实 HTTPS、终端和安装测试。
本地开发仍可使用动态 OpenSSL 和较新 glibc，但这类包需要显式传入
`--allow-shared-openssl --max-glibc VERSION`，不得当作兼容发布包。
AlmaLinux 的 Perl 构建依赖仍随仓库更新，不承诺跨日期生成的包逐字节一致。

1. 整理并提交变更，在 `pypto-tools/pto-btop` 建立独立仓库，保留上游来源与许可证。
2. 推送 `main`，确认两个架构都通过采集、HTTPS、终端及安装升级测试。
3. 确认 `VERSION` 和 `docs/cluster/releases/vVERSION.md` 一致，再推送版本标签。
4. 标签触发相同的构建、测试和打包。两个任务成功后，自动创建带两个安装包、
   校验文件、包含两个包固定校验和的 `setup.py` 和说明的 **draft release**；带 `-alpha` 等后缀的标签额外标记为 prerelease。
   重复标签不会覆盖已有 Release。
5. 检查草稿、两个架构的包清单与 SHA-256、实际机器兼容性检查结果后再发布。
   正式版本不带预发布后缀，并设为 Latest；预览版本保留 prerelease 标记。

本机参考截图在 `.gitignore` 中保留，仅使用模拟数据生成的 `detail-preview.png`
对外展示。上游原有 README、贡献规范和自动化配置归档于 `docs/upstream/`，
避免在独立仓库误触发原来的 Snap、macOS/BSD 或上游发布流程。
