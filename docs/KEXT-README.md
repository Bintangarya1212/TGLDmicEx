# TGLDmic kext package (Tiger Lake & Comet Lake / X1 Carbon Gen 7)

| File | What |
|---|---|
| `TGLDmic.kext` | Put in `EFI/OC/Kexts`, add to `Kernel > Add` after `Lilu.kext`, boot-arg `-tgldmiclilu` |
| `list-audio-devices.py` | List Core Audio devices: `python3 list-audio-devices.py` |
| `snapshot-to-wav.py` | For the `-tgldmiccap` boot-time capture test |
| `diff-regs.py` | Compare saved register dumps between two setups |

The native **Internal Microphone** device is provided by AppleHDA/AppleALC. TGLDmic hooks
`AppleHDAEngine::convertInputSamples` through Lilu and feeds it from the DSP ring; there is no
Core Audio plug-in or installer package. See `INSTALL.md`.

After installing, open **Audio MIDI Setup** and set the **Internal Microphone** format to
**48,000 Hz** (24-bit). The DSP is natively 48 kHz, so this avoids resampling and gives the best
recording quality.
