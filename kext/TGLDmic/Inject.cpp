// Phase 6: feed the DSP microphone into the HDA driver's own input engine.
//
// The native "Internal Microphone" device belongs to AppleHDA (or HDAUniversal on Tahoe), so its
// device object, controls, permissions and timestamps stay Apple's. What we replace is only the
// sample source: the codec's ADC (which is not wired to the PDM mics) delivers silence over the HDA
// link, and its DMA writes that silence into the engine's sample buffer.
//
// TGLDmic observes the engine's input stream descriptor. When it starts (someone records), we boot
// the DSP pipeline, then continuously overwrite the frames the DMA just wrote (the silence) with the
// DSP PCM, staying just behind the DMA write pointer (SD_LPIB). CoreAudio always reads behind that
// pointer (safety offset), so it only ever sees the phase-6 samples. Nothing in AppleHDA is patched
// or subclassed: the only shared state is host memory and the standard stream descriptor registers.
//
// Layout assumptions, all validated from the descriptor and logged:
//   * the engine's sample buffer is the descriptor's BDL target (linear concatenation of BDLEs),
//   * SD_LPIB is the byte position inside that ring and advances with the HDA link clock,
//   * SD_FMT describes the engine's hardware format (16/24/32-bit PCM, 1..8 ch, 44.1/48 kHz).
#include "TGLDmic.hpp"
#include "Regs.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/audio/IOAudioTypes.h>
#include <kern/clock.h>
#include <libkern/c++/OSNumber.h>
#include <string.h>

#define LOG(fmt, ...) logLine(fmt, ##__VA_ARGS__)

namespace {
constexpr uint32_t SD_CTL_RUN = 1u << 1;
constexpr uint32_t VS_DPIB_BASE = 0x1084, VS_DPIB_STRIDE = 0x20;
constexpr uint32_t kSrcFrameBytes = 16;                 // S32_LE x 4 ch from the DSP
constexpr uint32_t kHookSrcMargin = 256;                // source frames kept behind the DSP head (~5 ms)
constexpr uint32_t kHookDspResyncFrames = 2048;         // resync the smoothed DSP position beyond this (~43 ms)
const uint32_t kBitDepth[8] = {8, 16, 20, 24, 32, 0, 0, 0};

// Decode an HDA stream format value (SD_FMT): BASE is bit 14 (0 = 48 kHz, 1 = 44.1 kHz),
// MULT bits 13:11 (x1..x4), DIV bits 10:8 (1..8 divisor), BITS bits 6:4, CHAN bits 3:0 (+1).
uint32_t fmtRate(uint16_t fmt) {
    uint32_t base = (fmt & 0x4000) ? 44100 : 48000;
    uint32_t mult = ((fmt >> 11) & 0x7) + 1;
    uint32_t div = ((fmt >> 8) & 0x7) + 1;
    return base * mult / div;
}

} // namespace

bool TGLDmic::injectorStart(uint32_t idx) {
    if (injecting) return true;
    // Hook mode only: without a successful AppleHDA route and the internal-mic IOAudioStream there
    // is nothing to feed, so the DSP is not started at all.
    if (!hookInstalled || imicStreamCount == 0) {
        LOG("inject: Lilu hook %s - not injecting",
            !hookInstalled ? "not installed" : "has no internal-mic stream");
        return false;
    }

    uint32_t sd = sdOffset(idx);
    uint32_t ctl = r32(sd);
    if (!(ctl & SD_CTL_RUN)) return false;
    if (!ppOffset) return false;
    if (r32(ppOffset + HDA::PP_PPCTL) & (1u << idx)) return false;   // decoupled DSP stream

    // Hard identity check: never feed DSP samples into another input engine (headset mic, line-in).
    uint32_t tag = (ctl >> HDA::SD_CTL_TAG_SHIFT) & 0xF;
    if (!imicTag || tag != imicTag) {
        LOG("inject: descriptor %u (tag %u) is not the internal microphone (tag %u) - refusing",
            idx, tag, imicTag);
        return false;
    }

    uint32_t cbl = r32(sd + HDA::SD_CBL);
    uint16_t fmt = r16(sd + HDA::SD_FMT);
    uint32_t ch = (fmt & 0xF) + 1;
    uint32_t bits = kBitDepth[(fmt >> 4) & 0x7];
    uint32_t rate = fmtRate(fmt);
    if (!rate) rate = 48000;
    if (!cbl || !bits || !ch || ch > 8 || !(bits == 16 || bits == 20 || bits == 24 || bits == 32)) {
        LOG("inject: descriptor %u format unsupported (FMT 0x%04x -> %u ch, %u bit, %u Hz, CBL %u)",
            idx, fmt, ch, bits, rate, cbl);
        return false;
    }

    injectIdx = idx;
    injectTag = tag;
    injectFmt = fmt;
    injectChannels = ch;
    injectBits = bits;
    injectRate = rate;
    // 20/24-bit HDA samples live in 32-bit containers (spec 3.3.41); 16-bit uses 2 bytes.
    injectFrameBytes = ch * (bits == 16 ? 2 : 4);
    injectRingBytes = cbl;
    injectRingFrames = cbl / injectFrameBytes;
    if (!injectRingFrames) {
        LOG("inject: descriptor %u invalid CBL %u / frame %u", idx, cbl, injectFrameBytes);
        return false;
    }
    injectStep = ((uint64_t)kInjectRate << 32) / rate;                // 48000 / engine rate, 32.32
    LOG("inject: descriptor %u starts: tag %u FMT 0x%04x %u ch %u bit %u Hz (%u B/frame), CBL %u, LVI %u, LPIB %u",
        idx, injectTag, fmt, ch, bits, rate, injectFrameBytes, cbl, r16(sd + HDA::SD_LVI), r32(sd + HDA::SD_LPIB));

    // Power + DSP. holdPower() wakes the HDA driver so the controller leaves reset/D3.
    injectPowerHeld = holdPower();
    if (!injectPowerHeld) {
        LOG("inject: controller did not become usable - not injecting");
        return false;
    }
    if (!tryInit()) {
        LOG("inject: controller registers not readable - not injecting");
        releasePower();
        injectPowerHeld = false;
        return false;
    }
    bool wasAlive = dspAlive(false);
    injectBootedDsp = false;
    if (!wasAlive) {
        keepRunning = true;
        if (!bootDsp()) {
            LOG("inject: DSP boot failed (%s)", bootResult);
            keepRunning = false;
            releasePower();
            injectPowerHeld = false;
            return false;
        }
        injectBootedDsp = true;
    }
    bool wasStreaming = streaming;
    if (!pipelineStart()) {
        LOG("inject: pipeline start failed (%s)", captureResult);
        if (!wasStreaming && injectBootedDsp) { corePowerDown(); gating(true); keepRunning = false; }
        releasePower();
        injectPowerHeld = false;
        return false;
    }
    injectOwnsPipeline = !wasStreaming;

    hookDspInit = false;
    injecting = true;
    LOG("inject: streaming into input %u (tag %u), AppleHDA convertInputSamples hook", idx, injectTag);
    publish();
    return true;
}

void TGLDmic::injectorStop(const char *why) {
    if (!injecting) return;
    injecting = false;
    injectNextTry = samples + 10;   // 1 s backoff before watching for a new engine start
    // The hook runs on the audio engine's thread; wait for an in-flight fill before the DSP
    // pipeline is torn down and its core powered off.
    for (uint32_t i = 0; i < 20 && hookInFlight; i++) IOSleep(1);
    if (injectOwnsPipeline && (streaming || setupDone)) pipelineStop();
    // Re-check after the stop: if nothing else still needs the DSP, power it down again.
    bool dspBusyElsewhere = streaming;
    if (injectBootedDsp && !dspBusyElsewhere) {
        corePowerDown();
        gating(true);
        keepRunning = false;
    }
    if (injectPowerHeld && !dspBusyElsewhere) releasePower();
    injectOwnsPipeline = injectBootedDsp = injectPowerHeld = false;
    LOG("inject: stopped (%s): %llu fills, %llu frames",
        why, (unsigned long long)hookFills, (unsigned long long)hookFrames);
    publish();
}

bool TGLDmic::findImicTag() {
    OSDictionary *match = IOService::serviceMatching("AppleHDAEngineInput");
    if (!match) return false;
    OSIterator *it = IOService::getMatchingServices(match);
    match->release();
    if (!it) return false;
    bool found = false;
    while (OSObject *o = it->getNextObject()) {
        IOService *svc = OSDynamicCast(IOService, o);
        if (!svc) continue;
        IORegistryEntry *e = svc;
        bool ours = false;
        for (int depth = 0; e && depth < 8; depth++) {
            if (e == static_cast<IORegistryEntry *>(pci)) { ours = true; break; }
            e = e->getParentEntry(gIOServicePlane);
        }
        if (!ours) continue;
        OSString *desc = OSDynamicCast(OSString, svc->getProperty("IOAudioEngineDescription"));
        if (!desc || !desc->isEqualTo("DevShortNameIMic")) continue;
        OSString *uid = OSDynamicCast(OSString, svc->getProperty("IOAudioEngineGlobalUniqueID"));
        const char *s = uid ? uid->getCStringNoCopy() : nullptr;
        const char *colon = nullptr;
        for (const char *p = s; p && *p; p++)
            if (*p == ':') colon = p;
        if (!colon) continue;
        uint32_t tag = 0;
        for (const char *p = colon + 1; *p >= '0' && *p <= '9'; p++)
            tag = tag * 10 + (uint32_t)(*p - '0');
        if (!tag || tag > 15) continue;
        if (OSNumber *off = OSDynamicCast(OSNumber, svc->getProperty("IOAudioEngineInputSampleOffset")))
            if (off->unsigned32BitValue() > 0) imicSafetyFrames = off->unsigned32BitValue();
        imicTag = tag;
        if (svc != imicEngine || imicStreamCount == 0) cacheImicStreams(svc);
        LOG("inject: internal-mic engine %s -> HDA stream tag %u, safety offset %u frames, %u IOAudioStream(s)",
            s, tag, imicSafetyFrames, imicStreamCount);
        found = true;
        break;
    }
    it->release();
    return found;
}

// Remember the engine's input streams so the convertInputSamples hook can recognise them without a
// registry lookup in the audio path. IOAudioStream is an IOService child of its engine.
void TGLDmic::cacheImicStreams(IOService *engine) {
    if (engine) engine->retain();      // retain before dropping the old reference (may be the same object)
    releaseImicStreams();
    if (!engine) return;
    imicEngine = engine;
    OSIterator *kids = engine->getChildIterator(gIOServicePlane);
    if (!kids) return;
    while (OSObject *o = kids->getNextObject()) {
        IOService *s = OSDynamicCast(IOService, o);
        if (!s) continue;
        const OSMetaClass *mc = s->getMetaClass();
        const char *cn = mc ? mc->getClassName() : nullptr;
        if (!cn || strcmp(cn, "AppleHDAStream") != 0) continue;
        if (imicStreamCount >= sizeof(imicStreams) / sizeof(imicStreams[0])) break;
        s->retain();
        imicStreams[imicStreamCount++] = s;
    }
    kids->release();
}

void TGLDmic::releaseImicStreams() {
    for (uint32_t i = 0; i < imicStreamCount; i++) OSSafeReleaseNULL(imicStreams[i]);
    imicStreamCount = 0;
    OSSafeReleaseNULL(imicEngine);
}

// Called from the Lilu route of AppleHDAEngine::convertInputSamples on the audio engine's thread,
// for every input stream and client. Only the internal microphone's streams are touched, and only
// while the DSP pipeline is live. Nothing here allocates or takes a lock: it reads the HDA and DSP
// DMA positions, converts the requested frames from the DSP ring, and writes the CoreAudio buffer.
bool TGLDmic::hookFillClient(void *destBuf, uint32_t firstSampleFrame, uint32_t numSampleFrames,
                             const void *streamFormat, void *audioStream) {
    if (!injecting || !hda || !ringBuf.virt || !destBuf || !numSampleFrames)
        return false;
    bool ours = false;
    for (uint32_t i = 0; i < imicStreamCount; i++)
        if (static_cast<void *>(imicStreams[i]) == audioStream) { ours = true; break; }
    if (!ours) return false;

    const IOAudioStreamFormat *fmt = static_cast<const IOAudioStreamFormat *>(streamFormat);
    uint32_t ch = fmt ? fmt->fNumChannels : 2;
    if (!ch || ch > 8) ch = 2;
    // Mixable streams get a Float32 client buffer; non-mixable ones use the native 32-bit slot.
    bool toFloat = fmt ? (fmt->fIsMixable != 0) : true;

    if (numSampleFrames > injectRingFrames) numSampleFrames = injectRingFrames;
    __sync_add_and_fetch(&hookInFlight, 1);
    uint32_t sd = sdOffset(injectIdx);
    uint32_t ctl = r32(sd);
    uint32_t lpib = r32(sd + HDA::SD_LPIB);
    uint32_t dpibRaw = r32(VS_DPIB_BASE + VS_DPIB_STRIDE * capInIndex);
    bool live = ctl != 0xFFFFFFFF && (ctl & SD_CTL_RUN) &&
                (((ctl >> HDA::SD_CTL_TAG_SHIFT) & 0xF) == injectTag) &&
                lpib != 0xFFFFFFFF && dpibRaw != 0xFFFFFFFF;

    if (live) {
        uint32_t W = (lpib / injectFrameBytes) % injectRingFrames;

        // Smoothed DSP position (DPIB may only advance in period bursts).
        const uint64_t wrapQ = (uint64_t)kInjectSrcFrames << 32;
        uint64_t dpibQ = (uint64_t)((dpibRaw % kRingBytes) / kSrcFrameBytes) << 32;
        uint64_t now = mach_absolute_time();
        if (!hookDspInit) {
            hookDspQ = dpibQ;
            hookDspTime = now;
            hookDspInit = true;
        } else {
            uint64_t dns = 0;
            absolutetime_to_nanoseconds(now - hookDspTime, &dns);
            if (dns > 200000000ULL) {
                hookDspQ = dpibQ;
            } else {
                __uint128_t framesQ = ((__uint128_t)dns * kInjectRate) << 32;
                hookDspQ = (hookDspQ + (uint64_t)(framesQ / 1000000000ULL)) % wrapQ;
                int64_t delta = (int64_t)((dpibQ + wrapQ - hookDspQ) % wrapQ);
                if (delta > (int64_t)(wrapQ >> 1)) delta -= (int64_t)wrapQ;
                int64_t limit = (int64_t)kHookDspResyncFrames << 32;
                if (delta > limit || delta < -limit) hookDspQ = dpibQ;
            }
            hookDspTime = now;
        }
        const uint64_t dspQ = hookDspQ;
        const uint64_t marginQ = (uint64_t)kHookSrcMargin << 32;

        uint32_t first = firstSampleFrame % injectRingFrames;
        uint32_t dFirst = (W + injectRingFrames - first) % injectRingFrames;
        if (dFirst > injectRingFrames / 2) dFirst = 0;
        uint64_t distFirst = (uint64_t)dFirst * injectStep + marginQ;
        uint32_t need = (uint32_t)((distFirst >> 32) + (((uint64_t)numSampleFrames * injectStep) >> 32) + 4);
        if (need > kInjectSrcFrames) need = kInjectSrcFrames;
        uint32_t start = (uint32_t)(((dspQ + wrapQ - (distFirst % wrapQ)) % wrapQ) >> 32);
        uint32_t startByte = start * kSrcFrameBytes, bytes = need * kSrcFrameBytes;
        if (startByte + bytes <= kRingBytes) {
            ringBuf.invalidate(startByte, bytes);
        } else {
            ringBuf.invalidate(startByte, kRingBytes - startByte);
            ringBuf.invalidate(0, bytes - (kRingBytes - startByte));
        }

        const int32_t *src = reinterpret_cast<const int32_t *>(ringBuf.virt);
        const uint32_t gain = injectGainQ16;
        uint32_t p = first;
        for (uint32_t i = 0; i < numSampleFrames; i++) {
            uint32_t d = (W + injectRingFrames - p) % injectRingFrames;
            if (d > injectRingFrames / 2) d = 0;
            uint64_t off = (uint64_t)d * injectStep + marginQ;
            uint64_t q = (dspQ + wrapQ - (off % wrapQ)) % wrapQ;
            uint32_t si = (uint32_t)(q >> 32), frac = (uint32_t)q;
            const int32_t *a = src + si * 4;
            const int32_t *b = src + ((si + 1) % kInjectSrcFrames) * 4;
            int32_t s[4];
            for (int c = 0; c < 4; c++) {
                int64_t v;
                if (frac == 0) v = a[c];
                else v = (int64_t)a[c] + (((int64_t)(b[c] - a[c]) * (int64_t)frac) >> 32);
                if (gain != 65536) v = (v * (int64_t)gain) >> 16;
                if (v > 2147483647LL) v = 2147483647LL;
                if (v < -2147483648LL) v = -2147483648LL;
                s[c] = (int32_t)v;
            }
            if (toFloat) {
                float *out = reinterpret_cast<float *>(destBuf) + (uint64_t)i * ch;
                for (uint32_t c = 0; c < ch; c++) {
                    int32_t v = (ch == 1) ? (int32_t)(((int64_t)s[0] + s[1]) / 2) : s[c & 3];
                    out[c] = (float)v * (1.0f / 2147483648.0f);
                }
            } else {
                int32_t *out = reinterpret_cast<int32_t *>(destBuf) + (uint64_t)i * ch;
                for (uint32_t c = 0; c < ch; c++)
                    out[c] = (ch == 1) ? (int32_t)(((int64_t)s[0] + s[1]) / 2) : s[c & 3];
            }
            p = (p + 1) % injectRingFrames;
        }
        hookFrames += numSampleFrames;
        ++hookFills;
    } else {
        // Engine/tag not usable: silence for this request, never the print-through path.
        bzero(destBuf, (size_t)numSampleFrames * ch * 4);
    }
    __sync_sub_and_fetch(&hookInFlight, 1);
    return true;
}

// Called from the main (slow) timer every ~100 ms while the injector is enabled.
bool TGLDmic::injectorWatch() {
    if (!ready || !ppOffset || !memoryDecoded() || !inD0()) return false;
    if (injecting) {
        uint32_t sd = sdOffset(injectIdx);
        uint32_t ctl = r32(sd);
        if (ctl == 0xFFFFFFFF || !(ctl & SD_CTL_RUN)) { injectorStop("engine stopped (watch)"); return injecting; }
        // The hook has no timer tick of its own: check the DSP here (~2 s) and stop reading a dead
        // ring.
        if (hookInstalled && (samples % 10) == 0 && !dspAlive(false)) {
            injectorStop("DSP lost");
            injectNextTry = samples + 300;
            return injecting;
        }
        // AppleHDA may rebuild its IOAudioStream objects (format change, wake). If the hook has not
        // filled anything for ~5 s, refresh the identity list it matches against.
        if (hookInstalled && (samples % 25) == 0) {
            if (hookFills == hookFillsSeen) {
                if (imicEngine) cacheImicStreams(imicEngine);
                LOG("hook: no fills for 5 s - refreshed %u internal-mic stream(s)", imicStreamCount);
            } else {
                hookFillsSeen = hookFills;
                LOG("hook: %llu fills, %llu frames | engine W %u, DSP pos %u",
                    (unsigned long long)hookFills, (unsigned long long)hookFrames,
                    (r32(sdOffset(injectIdx) + HDA::SD_LPIB) / injectFrameBytes) % injectRingFrames,
                    (r32(VS_DPIB_BASE + VS_DPIB_STRIDE * capInIndex) % kRingBytes) / kSrcFrameBytes);
            }
        }
        return injecting;
    }

    // Descriptor indices move between boots; the internal mic's tag does not. Retry the lookup
    // every ~2 s until AppleHDA has published its engines.
    if ((!imicTag || imicStreamCount == 0) && samples >= imicSearchTick && !findImicTag())
        imicSearchTick = samples + 20;

    // Do not boot the DSP or touch any engine until the route is installed and the internal
    // microphone's IOAudioStream has been identified.
    if (!hookInstalled || imicStreamCount == 0) {
        if (!hookWaitLogged) {
            LOG("inject: waiting for the Lilu hook (%s) - not injecting",
                !hookInstalled ? "not installed" : "internal-mic stream not found");
            hookWaitLogged = true;
            publish();
        }
        return false;
    }

    // Gather the coupled input engines that are running right now. Only the descriptor whose tag
    // belongs to the internal microphone is ever eligible: when another input (the 3.5 mm headset
    // mic, line-in, ...) is the active one, its engine runs instead and TGLDmic must leave it alone.
    uint32_t runningMask = 0;
    int tagMatch = -1, running = 0;
    for (uint32_t i = 0; i < numIn; i++) {
        if ((int32_t)i == capInIndex) continue;                   // our own decoupled DSP stream
        uint32_t ctl = r32(sdOffset(i));
        if (ctl == 0xFFFFFFFF || !(ctl & SD_CTL_RUN)) continue;
        if (r32(ppOffset + HDA::PP_PPCTL) & (1u << i)) continue;  // decoupled -> DSP
        if (!r16(sdOffset(i) + HDA::SD_FMT)) continue;
        if (!r32(sdOffset(i) + HDA::SD_CBL)) continue;
        runningMask |= 1u << i;
        running++;
        uint32_t tag = (ctl >> HDA::SD_CTL_TAG_SHIFT) & 0xF;
        if (imicTag && tag == imicTag) tagMatch = (int)i;
    }
    injectCandidates = runningMask;
    if (!runningMask) return false;
    if (injectNextTry && samples < injectNextTry) return false;

    if (tagMatch < 0) {
        if (!notImicLogged) {
            LOG("inject: the internal microphone is not the active input (%d other input engine(s) running) - not injecting",
                running);
            notImicLogged = true;
            publish();
        }
        return false;
    }
    notImicLogged = false;
    int chosen = tagMatch;
    if (running > 1)
        LOG("inject: %d input engines running, choosing the internal microphone at descriptor %d (tag %u)",
            running, chosen, imicTag);
    if (!injectorStart((uint32_t)chosen)) {
        injectNextTry = samples + 100;   // back off 10 s before retrying a failing engine
        return false;
    }
    injectNextTry = 0;
    return injecting;
}
