<p align="center">
  <img src="assets/tgldmic-icon-256.png" width="200" alt="TGLDmic logo">
</p>

<h1 align="center">TGLDmic</h1>

<p align="center"><b>English</b> | <a href="README_CN.md">简体中文</a></p>

<p align="center"><a href="https://hits.sh/github.com/labrouss/TGLDmic/"><img src="https://hits.sh/github.com/labrouss/TGLDmic.svg?view=today-total&style=flat-square&label=visits" alt="visits"></a></p>

<p align="center"><b>Tiger Lake & Comet Lake microphone driver for macOS</b><br>
Internal digital microphones (DMIC) of Intel Tiger Lake and Comet Lake / Cannon Lake laptops (including ThinkPad X1 Carbon Gen 7) via the Intel Smart Sound DSP (SOF)</p>

> [!NOTE]
> **This fork is adapted for Comet Lake (CML) ThinkPad X1 Carbon Gen 7 (X1C7) from [labrouss/TGLDmic](https://github.com/labrouss/TGLDmic).** It introduces cAVS 1.8 platform support, dynamic core power gating, dual embedded Intel production SOF v2.2 firmware (`sof-cml.ri` & `sof-tgl.ri`), dynamic input stream allocation, and a Lilu hook that feeds the native Internal Microphone engine.

> Unofficial community project. Not affiliated with or endorsed by Intel, Apple or any laptop vendor.

## Background

Thanks to [labrouss](https://github.com/labrouss) and the contributors of the original TGLDmic for
their hard work on booting the Intel SOF firmware and bringing these DSP microphones to macOS.

The original design exposed the DSP stream through a user-space HAL (`AudioServerPlugIn`) virtual
microphone. In practice that meant:

- a HAL plug-in had to be installed into `/Library/Audio/Plug-Ins/HAL`;
- some built-in apps and system components (for example Safari) would not list or capture from the
  virtual device;
- the captured audio sounded dry and thin, with audible background noise.

This fork therefore reworks the exposure path. Instead of a virtual microphone it hooks
`AppleHDAEngine::convertInputSamples` through Lilu and feeds the **native Internal Microphone**
input engine directly from the DSP ring. No HAL plug-in is installed and no second audio device is
created.

On modern Intel laptops (Tiger Lake, Comet Lake, Whiskey Lake, etc.) the built-in digital microphones are not wired to the HDA codec but to the
Smart Sound DSP (cAVS), so AppleALC cannot make them work and hackintoshes end up without an internal mic.
TGLDmic boots Intel's signed SOF firmware on that DSP and feeds the native **Internal Microphone** input engine
through a Lilu hook on `AppleHDAEngine::convertInputSamples`. It runs next to AppleHDA/AppleALC, which keep
speakers and headset, and no second audio device is created.

## Features
- The native **Internal Microphone** device records DSP audio in System Settings and any recording app
- Stereo 48 kHz; software gain via `tgldmicgain=<percent>`
- DSP is powered only while something records (~120 ms start-up)
- Repeated recording sessions; coexists with AppleHDA/AppleALC
- Lilu hook on `AppleHDAEngine::convertInputSamples`; headset mic and line-in keep AppleHDA's own path
- Dual architecture support: Intel cAVS 2.5 (Tiger Lake) & cAVS 1.8 (Comet Lake / Cannon Lake / Coffee Lake)

## Compatibility
| | |
|---|---|
| Supported (Comet Lake / CFL) | Intel Smart Sound DSP `8086:02C8`: Comet Lake **LP** (10th-gen Core, e.g. **Lenovo ThinkPad X1 Carbon Gen 7** i7-10710U / i5-10210U)<br>Intel Smart Sound DSP `8086:9DC8`: Cannon Lake / Whiskey Lake **LP** (e.g. ThinkPad X1 Carbon Gen 7 i7-8565U)<br>Intel Smart Sound DSP `8086:06C8`, `8086:A348`: Comet Lake-H, Coffee Lake-H |
| Supported (Tiger Lake) | Intel Smart Sound DSP `8086:A0C8`: Tiger Lake **LP** (11th-gen Core, UP3/UP4)<br>Intel Smart Sound DSP `8086:43C8`: Tiger Lake **H** |
| Tested hardware | **Lenovo ThinkPad X1 Carbon Gen 7** (Comet Lake i7-10710U, 4 PDM mics, ALC285)<br>**HP EliteBook 840 G8** (Tiger Lake i5-1135G7, 4 PDM mics, ALC285) |
| macOS | Ventura, Sonoma, Sequoia, Tahoe tested / supported |
| Audio driver | Works next to **AppleALC/AppleHDA** or **HDAUniversal** (speakers and headset stay with them) |

> **Device-id spoof:** OpenCore configs usually spoof the audio controller as `34C8` or `9DC8` for AppleALC.
> TGLDmic matches those spoof IDs too, detects the underlying hardware platform family, and boots the matching Intel-signed SOF firmware (`sof-cml.ri` for Comet Lake or `sof-tgl.ri` for Tiger Lake).

## Install
Download the kext archive from [Releases](../../releases):

| File | What |
|---|---|
| `TGLDmic-<version>-kext.zip` | `TGLDmic.kext` for OpenCore + diagnostic tools |

1. Add `TGLDmic.kext` to `EFI/OC/Kexts` and to `Kernel > Add` in `config.plist`, after `Lilu.kext`.
   The kext carries an early `IOResources` personality, so it registers with Lilu before the
   plugin-registration window closes even though the PCI bus is enumerated later.
2. Add `-tgldmiclilu` (or `-tgldmicinject`) to `boot-args` and reboot.
3. Pick **Internal Microphone** under **System Settings → Sound → Input**.
4. Open **Audio MIDI Setup** and set the **Internal Microphone** format to **48,000 Hz**
   (24-bit recommended). This matches the DSP's native 48 kHz and avoids host resampling, giving
   the best recording quality.

> **v0.6.4:** with `-tgldmiclilu` (or `-tgldmicinject`) Lilu routes
> `AppleHDAEngine::convertInputSamples` and TGLDmic fills the native **Internal Microphone**
> engine's Core Audio buffer directly from the DSP ring. No second device or plug-in is needed and
> the original converter is not called for the internal mic. External inputs (headset mic/line-in)
> still use AppleHDA's own path. See [docs/INSTALL.md §2](docs/INSTALL.md).

Full steps, troubleshooting and removal: [docs/INSTALL.md](docs/INSTALL.md).

## How it works
```
 Apps ─▶ Core Audio ─▶ AppleHDA input engine ─▶ convertInputSamples hook ─▶ DSP ring ─▶ TGLDmic.kext ─▶ SOF DSP ─▶ PDM mics
```
The kext loads the Intel-signed SOF firmware into the DSP, replays the IPC messages Linux uses to
build the DMIC capture pipeline, and streams 48 kHz/4-channel PCM into its ring buffer by DMA. Lilu
routes AppleHDA's input conversion; for the internal microphone the hook fills the Core Audio buffer
from that ring and returns. Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Your laptop is not working?
The pipeline setup is recorded from the reference machine. If the mic stays silent on your laptop:
1. Boot an Ubuntu live USB and run `sudo bash linux/capture-sof-dmic.sh`. It records no audio into the tarball.
2. Open an issue with the tarball and `ioreg -l -w0 -c TGLDmic` from macOS.

Debug boot args and test procedures: [docs/TESTING.md](docs/TESTING.md).

## Building
GitHub Actions builds the kext on every push and attaches it to a Release for `v*` tags.
Locally on macOS with Xcode command-line tools, [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK)
and a Lilu 1.7.x source tree next to the repository (the Makefile uses `../Lilu.kext/Contents/Resources`
for the plugin headers and `plugin_start.cpp`):

    make -C kext SDK=/path/to/MacKernelSDK

`tools/set-version.sh X.Y.Z` sets the version in every place.

| Path | What |
|---|---|
| `kext/` | Kernel driver `TGLDmic.kext` |
| `firmware/` | Intel-signed SOF firmware embedded in the kext |
| `linux/` | Capture tool and IPC extractor |
| `captures/` | Reference captures (logs, NHLT, topology; no audio) |
| `tools/` | Build and diagnostic helpers |
| `docs/` | Install, testing, architecture |

## Post-optimization
1. DSP power control: currently it seems to use a 100ms timer polling approach. Although there's no performance issue, it's not very elegant; later we'll hook AppleHDA's microphone start/stop control API to handle power elegantly.
2. Microphone mute is not supported; this may be related to DSP power management.
3. The SOF firmware defaults to 48kHz sampling; 44.1 and 96 are not supported. If SOF supports setting the DSP sample rate, we can hook the microphone sample rate setting to achieve dynamic sample rate adjustment.
4. In the Apple SDK, convertInputSamples has already been marked as deprecated; we plan to change it to hook convertInputSamplesVBR.

## Credits
- [labrouss](https://github.com/labrouss) - for the original TGLDmic
- [Sound Open Firmware](https://github.com/thesofproject) — the DSP firmware and the Linux driver this work is based on
- [acidanthera](https://github.com/acidanthera) — MacKernelSDK, OpenCore, AppleALC

## Licence
TGLDmic is BSD-3-Clause ([LICENSE](LICENSE)). The embedded SOF firmware is Intel's, redistributed
unmodified under [firmware/LICENCE.Intel](firmware/LICENCE.Intel).
