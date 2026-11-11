<p align="center">
  <img src="assets/tgldmic-icon-256.png" width="200" alt="TGLDmic logo">
</p>

<h1 align="center">TGLDmic</h1>

<p align="center"><b>简体中文</b> | <a href="README.md">English</a></p>

<p align="center"><a href="https://hits.sh/github.com/labrouss/TGLDmic/"><img src="https://hits.sh/github.com/labrouss/TGLDmic.svg?view=today-total&style=flat-square&label=visits" alt="visits"></a></p>

<p align="center"><b>面向 macOS 的 Tiger Lake 与 Comet Lake 麦克风驱动</b><br>
通过 Intel Smart Sound DSP（SOF）驱动 Intel Tiger Lake 与 Comet Lake / Cannon Lake 笔记本（包括 ThinkPad X1 Carbon Gen 7）的内置数字麦克风（DMIC）</p>

> [!NOTE]
> **本分支基于 [labrouss/TGLDmic](https://github.com/labrouss/TGLDmic)，针对 Comet Lake（CML）ThinkPad X1 Carbon Gen 7（X1C7）适配。** 新增 cAVS 1.8 平台支持、动态核心电源门控、双份内嵌 Intel 生产签名 SOF v2.2 固件（`sof-cml.ri` 与 `sof-tgl.ri`）、动态输入流分配，以及将原生 Internal Microphone 引擎接入 DSP 的 Lilu Hook。

> 非官方社区项目，与 Intel、Apple 或任何笔记本厂商无关，也未获其背书。

## 背景

首先感谢原作者 [labrouss](https://github.com/labrouss) 以及原版 TGLDmic 的贡献者，感谢他们在启动
Intel SOF 固件、让这些 DSP 麦克风在 macOS 上工作方面付出的辛苦努力。

原版方案通过用户态的 HAL（`AudioServerPlugIn`）虚拟麦克风来暴露 DSP 音频流。实际使用中存在这些问题：

- 需要在 `/Library/Audio/Plug-Ins/HAL` 安装 HAL 插件；
- 部分内置应用和系统组件（例如 Safari）无法列出或调用这个虚拟设备；
- 录到的声音干涩、不饱满，并且有可闻的底噪。

因此本分支重做了音频输出路径：不再创建虚拟麦克风，而是通过 Lilu Hook
`AppleHDAEngine::convertInputSamples`，把 DSP ring 直接送入 **原生的 Internal Microphone
输入引擎**。不需要安装 HAL 插件，也不会出现第二个音频设备。

在现代 Intel 笔记本（Tiger Lake、Comet Lake、Whiskey Lake 等）上，内置数字麦克风并未接到 HDA codec，而是接在
Smart Sound DSP（cAVS）上，因此 AppleALC 无法让它们工作，黑苹果通常没有内置麦克风。
TGLDmic 在该 DSP 上启动 Intel 签名的 SOF 固件，并通过 Lilu 对 `AppleHDAEngine::convertInputSamples`
的 Hook，把音频送入原生的 **Internal Microphone** 输入引擎。它与 AppleHDA/AppleALC 共存
（扬声器和耳机仍由它们负责），不会创建第二个音频设备。

## 特性
- 原生 **Internal Microphone** 设备可在系统设置和任意录音应用中使用
- 立体声 48 kHz；通过 `tgldmicgain=<百分比>` 设置软件增益
- 仅在有录音时给 DSP 供电（启动约 120 ms）
- 支持反复录音；与 AppleHDA/AppleALC 共存
- Lilu Hook `AppleHDAEngine::convertInputSamples`；外接耳机麦和 Line In 仍走 AppleHDA 原有路径
- 双架构支持：Intel cAVS 2.5（Tiger Lake）与 cAVS 1.8（Comet Lake / Cannon Lake / Coffee Lake）

## 兼容性
| | |
|---|---|
| 支持（Comet Lake / CFL） | Intel Smart Sound DSP `8086:02C8`：Comet Lake **LP**（第 10 代 Core，例如 **Lenovo ThinkPad X1 Carbon Gen 7** i7-10710U / i5-10210U）<br>Intel Smart Sound DSP `8086:9DC8`：Cannon Lake / Whiskey Lake **LP**（例如 ThinkPad X1 Carbon Gen 7 i7-8565U）<br>Intel Smart Sound DSP `8086:06C8`、`8086:A348`：Comet Lake-H、Coffee Lake-H |
| 支持（Tiger Lake） | Intel Smart Sound DSP `8086:A0C8`：Tiger Lake **LP**（第 11 代 Core，UP3/UP4）<br>Intel Smart Sound DSP `8086:43C8`：Tiger Lake **H** |
| 已测试硬件 | **Lenovo ThinkPad X1 Carbon Gen 7**（Comet Lake i7-10710U，4 个 PDM 麦克风，ALC285）<br>**HP EliteBook 840 G8**（Tiger Lake i5-1135G7，4 个 PDM 麦克风，ALC285） |
| macOS | 已在 Ventura、Sonoma、Sequoia、Tahoe 测试 / 支持 |
| 音频驱动 | 可与 **AppleALC/AppleHDA** 或 **HDAUniversal** 共存（扬声器和耳机仍由它们处理） |

> **Device-id spoof：** OpenCore 配置通常会把音频控制器伪装成 `34C8` 或 `9DC8` 供 AppleALC 使用。
> TGLDmic 也匹配这些伪装 ID，会识别底层硬件平台并启动对应的 Intel 签名 SOF 固件
> （Comet Lake 用 `sof-cml.ri`，Tiger Lake 用 `sof-tgl.ri`）。

## 安装
从 [Releases](../../releases) 下载 kext 压缩包：

| 文件 | 说明 |
|---|---|
| `TGLDmic-<version>-kext.zip` | 用于 OpenCore 的 `TGLDmic.kext` + 诊断工具 |

1. 将 `TGLDmic.kext` 放入 `EFI/OC/Kexts`，并在 `config.plist` 的 `Kernel > Add` 中加到
   `Lilu.kext` 之后。kext 带有一个早期的 `IOResources` personality，因此即使 PCI 总线稍后才
   枚举，它也能在 Lilu 关闭插件注册窗口之前完成注册。
2. 在 `boot-args` 中加入 `-tgldmiclilu`（或 `-tgldmicinject`），重启。
3. 在 **系统设置 → 声音 → 输入** 中选择 **Internal Microphone**。
4. 打开 **音频 MIDI 设置**（应用程序 → 实用工具），选择 **Internal Microphone** 输入设备，
   把格式设置为 **48,000 Hz**（建议 24-bit）。DSP 原生就是 48 kHz，这样无需主机侧重采样，
   拾音质量最佳；44.1/96 kHz 会走线性重采样，可能出现粗糙感。

> **v0.6.4：** 使用 `-tgldmiclilu`（或 `-tgldmicinject`）时，Lilu 会路由
> `AppleHDAEngine::convertInputSamples`，TGLDmic 直接从 DSP ring 填充原生 **Internal Microphone**
> 引擎的 Core Audio 缓冲区。不需要第二个设备或插件，内置麦路径不再调用原转换函数；外接输入
> （耳机麦 / Line In）仍走 AppleHDA 自己的路径。见 [docs/INSTALL.md](docs/INSTALL.md)。

完整步骤、排错和卸载：[docs/INSTALL.md](docs/INSTALL.md)。

## 工作原理
```
 应用 ─▶ Core Audio ─▶ AppleHDA 输入引擎 ─▶ convertInputSamples Hook ─▶ DSP ring ─▶ TGLDmic.kext ─▶ SOF DSP ─▶ PDM 麦克风
```
kext 将 Intel 签名的 SOF 固件加载到 DSP，回放 Linux 用于建立 DMIC 采集管线的 IPC 消息，并通过
DMA 把 48 kHz/4 声道 PCM 写入自己的 ring buffer。Lilu 路由 AppleHDA 的输入转换；对内置麦克风，
Hook 从该 ring 填充 Core Audio 缓冲区并返回。详见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 你的笔记本不工作？
管线配置是从参考机器上录制的。如果麦克风在你的笔记本上没有声音：
1. 启动 Ubuntu Live USB，运行 `sudo bash linux/capture-sof-dmic.sh`。该脚本不会把音频写进压缩包。
2. 带上压缩包以及 macOS 下的 `ioreg -l -w0 -c TGLDmic` 提交 issue。

调试 boot-args 和测试流程：[docs/TESTING.md](docs/TESTING.md)。

## 构建
GitHub Actions 会在每次 push 时构建 kext，并在 `v*` tag 时附加到 Release。
本地 macOS 构建需要 Xcode 命令行工具、[MacKernelSDK](https://github.com/acidanthera/MacKernelSDK)，
以及放在仓库旁边的 Lilu 1.7.x 源码树（Makefile 使用 `../Lilu.kext/Contents/Resources` 中的
插件头文件和 `plugin_start.cpp`）：

    make -C kext SDK=/path/to/MacKernelSDK

`tools/set-version.sh X.Y.Z` 会更新所有位置的版本号。

| 路径 | 内容 |
|---|---|
| `kext/` | 内核驱动 `TGLDmic.kext` |
| `firmware/` | 内嵌到 kext 中的 Intel 签名 SOF 固件 |
| `linux/` | 采集工具与 IPC 提取器 |
| `captures/` | 参考采集数据（日志、NHLT、topology；不含音频） |
| `tools/` | 构建与诊断辅助脚本 |
| `docs/` | 安装、测试、架构文档 |

## 致谢
- [labrouss](https://github.com/labrouss) — 原始 TGLDmic
- [Sound Open Firmware](https://github.com/thesofproject) — DSP 固件以及本项目所基于的 Linux 驱动
- [acidanthera](https://github.com/acidanthera) — MacKernelSDK、OpenCore、AppleALC

## 许可证
TGLDmic 采用 BSD-3-Clause（[LICENSE](LICENSE)）。内嵌的 SOF 固件版权归 Intel，按原样再分发，
见 [firmware/LICENCE.Intel](firmware/LICENCE.Intel)。
