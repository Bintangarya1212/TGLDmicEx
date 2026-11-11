// Version string shared by the kext and the build tooling.
#pragma once

#define kTGLDmicVersionString "0.6.4"   // kept in sync with Info.plist by tools/set-version.sh
// DSP ring format: S32_LE, 4 interleaved channels, 48 kHz. ch0/ch1 = mic pair A, ch2 == ch3 = mic B.
