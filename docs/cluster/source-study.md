# btop 源码调研与参考材料

## 固定源码基线

| 项目 | 记录 |
| --- | --- |
| 上游 | https://github.com/aristocratos/btop |
| 提交 | `e28a2701c7208ff00ab780a68287e2204f356ba0` |
| 提交时间 | 2026-11-03T11:46:17+00:00 |
| 提交主题 | chore(deps): update zizmorcore/zizmor-action action to v0.8.1 |
| 获取日期 | 2026-11-04 |
| 内部版本字符串 | `1.4.8`，见 `src/btop.cpp:97` |
| 获取方式 | 上游 main 浅克隆，深度 1；完整仓库文件已落地，完整历史未下载 |

以下行号对应上述提交，不能套用到其他 main 版本。需要查看更早历史时再获取所需提交；设计材料不把版本字符串当作发布标签。机器可读记录见 [upstream.json](upstream.json)。

## 源码入口与结论

| 文件与位置 | 可核实的现状 | 对本项目的影响 |
| --- | --- | --- |
| [CMakeLists.txt](../../CMakeLists.txt):6、26、46 | CMake ≥3.25、C++23；`btop` 可执行文件链接 OBJECT 库 `libbtop` | 可以增加目标，但 libbtop 并非已解耦的远程监控 SDK |
| [README.md](../../README.md):456 | 当前 Linux 编译说明要求 GCC ≥14 或 Clang ≥19 | 不能按旧版 btop 的 GCC 11 说明构建当前提交；标准库特性也必须满足 |
| [src/main.cpp](../../src/main.cpp):11 | main 转交 `btop_main` | 新 agent 采用独立 main，绕开本地 TUI 初始化 |
| [src/btop.cpp](../../src/btop.cpp):538、592、612、632 | Runner 按顺序调用本地 collect 与 draw | 新客户端应从快照存储绘制，不能继续采集观察机的指标冒充远端 |
| [src/btop.cpp](../../src/btop.cpp):1005、1031 | 初始化终端和平台共享状态 | agent 需要单独的无界面启动／退出生命周期 |
| [src/btop_draw.hpp](../../src/btop_draw.hpp):82、90、104 | createBox、Meter、Graph 提供字符绘图接口 | 原版视觉组件的主要复用入口 |
| [src/btop_draw.cpp](../../src/btop_draw.cpp):545、567、2248 | CPU 面板持有全局尺寸、graphs、meters；calcSizes 统一布局 | 新建每主机绘图上下文，避免四张卡片覆盖同一缓存 |
| [src/btop_shared.hpp](../../src/btop_shared.hpp):208、265、312、362 | 各模块同时暴露指标、界面状态、collect 和 draw | 协议模型独立设计，不直接复制整套 struct |
| [src/linux/btop_collect.cpp](../../src/linux/btop_collect.cpp):148、2317、2881、3087 | current_cpu、current_mem、current_net、current_procs 为进程内共享状态 | 每台机器一个 agent 可以隔离采集状态；客户端仍需 HostSnapshot |
| [src/linux/btop_collect.cpp](../../src/linux/btop_collect.cpp):332 | Shared::init 读取 proc、初始化 CPU／内存／GPU 等 | 首版拆出 CPU／内存必要初始化，GPU 默认关闭 |
| [src/linux/btop_collect.cpp](../../src/linux/btop_collect.cpp):1185、2411、2991 | 历史长度受面板 width 影响 | headless collector 不能直接依赖默认 UI 尺寸；历史移到客户端 |
| [src/linux/btop_collect.cpp](../../src/linux/btop_collect.cpp):3206 | 进程采集同时读取排序、过滤、树视图等全局配置 | 提取进程扫描与差分核心，协议排序／过滤使用显式请求参数 |
| [src/btop_menu.cpp](../../src/btop_menu.cpp):1038、1162 | 菜单直接对 PID 调用本机 kill | 远程详情不能复用这些动作；首版只读输入路径独立 |
| [src/btop_config.cpp](../../src/btop_config.cpp):76、274 | 内置主题配置与默认值 | 复用视觉选项时使用独立配置文件，防止覆盖已有 btop 配置 |
| [tests/CMakeLists.txt](../../tests/CMakeLists.txt):4 | 测试使用 GTest v1.17.0，经 FetchContent 获取 | 无网构建需预置依赖；新增测试接入同一框架 |

上述表格描述未修改的固定上游提交，当前工作树行号已经发生变化。绘图提取现已完成链接验证：新二进制没有 Cpu／Mem／Net／Proc／Config 的实现符号，原版兼容性测试通过。无终端 collector 现已实现于 `src/collector/`，CPU／内存采用原版计数口径，以实例持有差分状态；原版 Linux collector 保留。agent 的符号检查未发现 Draw／Theme／Term／Config 实现。

## 原版视觉材料

仓库已包含上游截图和主题，无需另存网络图片：

- [常规布局截图](../../Img/normal.png)：单机详情的分区和曲线风格参考。
- [替代布局截图](../../Img/alt.png)：窄屏布局和配色参考。
- [TTY 截图](../../Img/tty.png)：低色彩／字符回退参考。
- [主题目录](../../themes/)：直接复用 `.theme` 格式。
- [帮助页截图](../../Img/help-menu.png)：按键提示的组织方式参考。

这些是原版截图，不是已经完成的多机界面。多机布局示意在[设计方案](design.md)中。

## 许可证与来源保留

btop 主项目使用 [Apache License 2.0](../../LICENSE)。衍生版本分发时保留许可证及适用的版权／归属声明，在修改文件中注明修改；上游发行内容若附 NOTICE，保留适用声明。该许可证不要求把自己的修改公开，也不授予冒用原项目商标的权限。

当前快照还有 [fmt 许可证](../../include/fmt/LICENSE.rst) 和 `src/linux/intel_gpu_top/` 内各文件的第三方版权／许可头。首版关闭 GPU 构建不会自动消除分发源码时的声明要求。提取源码时保留文件头，增加明确的修改说明；新增本项目文件采用 Apache-2.0 SPDX 标记。发布前按实际打包内容核对，不把所有第三方代码统称为 Apache-2.0。

拟用名称 `btop-cluster` 表示基于 btop 的衍生项目，文档注明来源。上游 [CONTRIBUTING.md](../../CONTRIBUTING.md) 对提交其仓库的代码和 PR 文本另有要求；本阶段在本地衍生仓库实现和验证，不向上游提交 PR。

## 官方参考资料

| 资料 | 用途 |
| --- | --- |
| [固定版本源码](https://github.com/aristocratos/btop/tree/e28a2701c7208ff00ab780a68287e2204f356ba0) | 保持设计对应到可复查的代码 |
| [固定版本许可证](https://github.com/aristocratos/btop/blob/e28a2701c7208ff00ab780a68287e2204f356ba0/LICENSE) | 修改与分发条件 |
| [NetBird SSH](https://docs.netbird.io/manage/peers/ssh) | 区分网络互通、内置 SSH 与身份认证，内置 SSH 需单独启用 |
| [OpenSSH ssh 手册](https://man.openbsd.org/ssh.1) | 无 PTY 执行远程程序、stdin/stdout、远程命令参数语义 |
| [Linux proc 文档](https://www.kernel.org/doc/html/latest/filesystems/proc.html) | CPU、内存、进程等底层数据定义与访问约束 |
| [nlohmann/json 3.11.3](https://github.com/nlohmann/json/releases/tag/v3.11.3) | 当前已验证的系统 JSON 依赖，使用其 CMake 包 |

设计最初记录了 3.12.0 候选及官方单头文件校验和，但未下载；当前实现使用系统已安装的 nlohmann/json 3.11.3，通过 `find_package(nlohmann_json 3.11 REQUIRED)` 引入。`upstream.json` 同时保留候选来源和实际版本。协议已实现深度／帧长度限制、重复键拒绝和 uint64 解析，发布包需保留实际分发依赖的 MIT 许可。
