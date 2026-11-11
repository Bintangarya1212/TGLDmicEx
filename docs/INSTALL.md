# TGLDmic: internal microphone for Intel Tiger Lake & Comet Lake laptops (macOS)

Supported laptops:
- **Lenovo ThinkPad X1 Carbon Gen 7** (Comet Lake i7-10710U / i7-10510U, Whiskey Lake i7-8565U / i5-8265U) with ALC285 & 4-mic array
- **HP EliteBook 840 G8** (Tiger Lake i5-1135G7 / i7-1165G7) with ALC285 & 4-mic array
- Other Intel Tiger Lake (`8086:A0C8`, `8086:43C8`) and Comet Lake / Whiskey Lake / Coffee Lake (`8086:02C8`, `8086:9DC8`, `8086:A348`) laptops with PDM microphone arrays on the Intel cAVS audio DSP.

Each build produces one file:

| File | What |
|---|---|
| `TGLDmic-<version>-kext.zip` | `TGLDmic.kext` (kernel driver) + diagnostic scripts |

## 1. Kext (OpenCore)
1. Copy `TGLDmic.kext` to your EFI: `EFI/OC/Kexts/TGLDmic.kext`.
2. In `config.plist` -> `Kernel` -> `Add`, add `TGLDmic.kext` after `Lilu.kext`:
   - `BundlePath`: `TGLDmic.kext`
   - `ExecutablePath`: `Contents/MacOS/TGLDmic`
   - `PlistPath`: `Contents/Info.plist`
   - `Enabled`: `true`
   The kext carries an early `IOResources` personality (`TGLDmicEarly`), so it registers with Lilu
   before Lilu closes its plugin-registration window even though the PCI device is matched later.
   Lilu must be loaded first; the position of AppleALC is not critical.
3. In `NVRAM` -> `Add` -> `7C436110-AB2A-4BBB-A880-FE41995C9F82` -> `boot-args`, add
   `-tgldmiclilu` (or `-tgldmicinject`; they are equivalent). Optional: `tgldmicgain=<percent>`.
   The old `-tgldmic` plug-in mode and `tgldmicinjtick` / `tgldmicinject=<idx>` no longer exist.
4. Reboot and select the native **Internal Microphone** in **System Settings → Sound → Input**.

## 2. Native "Internal Microphone" hook (v0.6.4+)

TGLDmic hooks `AppleHDAEngine::convertInputSamples` through Lilu. For the internal microphone's
`IOAudioStream` it fills the Core Audio client buffer directly from the DSP ring (48 kHz, 4 ch,
S32_LE → Float32 at the engine rate/channel count) and returns without calling the original
converter. Other inputs (headset mic, line-in) still use AppleHDA's own conversion. There is no
kernel timer and no write into the HDA DMA ring.

1. Requirements: Lilu 1.7.x loaded, `TGLDmic.kext` in `EFI/OC/Kexts` and in `Kernel → Add` after
   `Lilu.kext`. The kext carries an early `IOResources` personality (`Info.plist` → `TGLDmicEarly`);
   it is what lets the Lilu handshake complete before Lilu finalises plugin registration.
2. `boot-args`: `-tgldmiclilu` (or `-tgldmicinject`, they are equivalent) instead of `-tgldmic`.
   - `tgldmicgain=<percent>` applies software gain (default 100, max 400).
   - `tgldmicinjtick` and `tgldmicinject=<idx>` no longer exist: the timer and forced-descriptor
     code were removed.
3. Reboot and select the native **Internal Microphone** in **System Settings → Sound → Input**.
4. Open **Audio MIDI Setup** (Applications → Utilities), select the **Internal Microphone** input
   device, and set its format to **48,000 Hz** (24-bit recommended). The DSP pipeline is natively
   48 kHz, so this avoids host resampling and gives the best recording quality; 44.1/96 kHz fall
   back to linear resampling and can sound rough.
5. Verify while recording:

       ioreg -l -w0 -c TGLDmic | grep -E '"(LiluHook|HookReqAccess|HookRouteOk|HookFills|HookFrames|InjectState|Streaming|InjectRate|InjectFormat)"'

   `HookReqAccess`, `HookPluginStart`, `HookOnKextErr`, `HookRouteOk` must be 1; `LiluHook` must be
   `Yes`; `HookFills`/`HookFrames` must rise; `InjectState` = 2 and `Streaming` = Yes.

   The `Log` property (or `log show ... CONTAINS "TGLDmic"`) shows:

       inject: descriptor 0 starts: tag 1 FMT 0x... 2 ch 24 bit 48000 Hz (8 B/frame), CBL 196608 ...
       inject: streaming into input 0 (tag 1), AppleHDA convertInputSamples hook
       hook: N fills, N frames | engine W ..., DSP pos ...

### If it does not work

| Symptom | Meaning |
|---|---|
| `HookReqAccess` = 7 | `TooLate`: the kext's kmod entry ran after Lilu closed plugin registration. Check that the early `TGLDmicEarly` (`IOResources`) personality is present in `Info.plist`. |
| `HookReqAccess` = 8 | `Offline`: the Lilu API was not initialised when the kext entry ran; check Lilu load order/version. |
| `LiluHook` = No, `HookReqAccess` = 1 | Lilu registered but routing failed (`HookRouteOk` = 2). Check the AppleHDA load callback (`HookKextCb`) and that AppleHDA is present. |
| `InjectState` stays 1 | The native mic's coupled input engine is not running (another default input device, or AppleHDA has no internal mic). Only the descriptor carrying the internal-mic tag is accepted. |
| `HookFills` stays 0 while recording | `convertInputSamples` is not being called for the internal mic, or the route/stream identity did not match. Report `HookRouteOk`, `ImicStreams`, and the running input mask. |
| 44.1/96 kHz sounds wobbly | Known limitation: host-side resampling from the DSP's native 48 kHz is linear, and 44.1 kHz is not a native DMIC rate. 48 kHz is exact. |

Note: AppleHDA must actually create the native microphone device for this mode (it depends on
AppleALC's layout). The hook does not create or rename devices.

## Logs if something does not work
More in [TESTING.md](TESTING.md).

    python3 list-audio-devices.py      # list Core Audio input devices
    log show --last boot --predicate 'eventMessage CONTAINS "TGLDmic"' --info
    ioreg -l -w0 -c TGLDmic          # "Log" property: the kext's last 128 lines

## Known limits
* The DSP is powered only while something records: the first ~120 ms of each new recording session
  boot the DSP. While recording, TGLDmic holds the HDA PCI device in D0 (IOKit power child), so the
  HDA driver's idle power-down cannot reset the DSP mid-stream.
* System sleep while recording stops the stream cleanly; recording again after wake re-boots the DSP.
* Tested on Ventura (AppleALC) and Tahoe (HDAUniversal). Sonoma and Sequoia not yet verified.
* Core Audio IO buffers above ~2048 frames could read outside the 4096-frame ring; normal apps use 512–1024.

## Remove
Delete `TGLDmic.kext` from `EFI/OC/Kexts` and from `Kernel → Add`, and remove
`-tgldmiclilu` / `-tgldmicinject` from `boot-args`. There is no plug-in or installer package to
uninstall.
