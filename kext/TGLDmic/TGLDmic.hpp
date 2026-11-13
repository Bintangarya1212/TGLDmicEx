#pragma once
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOMessage.h>
#include "DmaBuffer.hpp"

struct IpcMsg;   // IpcReplay.hpp

// Hook-registration diagnostics shared with TGLDmicLilu.cpp and published by TGLDmic::publish().
// Error/result fields use +1 encoding so 0 means "not reached".
struct TGLDmicHookStatus {
    uint32_t kmodEntry, reqAccess, shouldLoad, started, arg, onKextErr, kextCb, routeOk;
};
extern TGLDmicHookStatus tgldmicHookStatus;

// TGLDmic: Digital Microphone driver for Intel Tiger Lake (cAVS 2.5) and
// Comet Lake / Cannon Lake / Coffee Lake (cAVS 1.8), including ThinkPad X1 Carbon Gen 7.
// Matches the HDA controller next to AppleHDA (own IOMatchCategory, provider is never opened),
// boots Intel SOF production-signed firmware on the audio DSP and streams 48 kHz DMIC audio.
class TGLDmic : public IOService {
    OSDeclareDefaultStructors(TGLDmic)
public:
    enum PlatformFamily : uint32_t {
        kPlatformUnknown   = 0,
        kPlatformTigerLake  = 1, // Intel Tiger Lake LP/H (cAVS 2.5)
        kPlatformCometLake  = 2, // Intel Comet Lake / Cannon Lake / Coffee Lake (cAVS 1.8)
    };

    static constexpr uint32_t kRingBytes = 0x10000;   // DSP host DMA ring
    IOService *probe(IOService *provider, SInt32 *score) override;
    IOReturn setPowerState(unsigned long state, IOService *device) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    PlatformFamily getPlatformFamily() const { return platformFamily; }
    const char *platformName() const;

    // ---- Lilu hook mode (-tgldmiclilu) ----
    // The injected DSP samples are no longer copied by a kernel timer racing the HDA DMA. Instead
    // Lilu routes IOAudioStream::readInputSamples in IOAudioFamily; the internal microphone's copy
    // path fills the engine's ring with DSP samples immediately before AppleHDA converts them to the
    // client format. gInstance is the live service the hook talks to (set in start, cleared in stop).
    static TGLDmic *instance() { return gInstance; }
    static void setHookInstalled(bool ok) { hookInstalled = ok; }
    static bool hookIsInstalled() { return hookInstalled; }
    // Fill the CoreAudio client buffer directly from the DSP ring. Returns true when the request was
    // handled, in which case the hook returns without calling the original AppleHDA converter.
    bool hookFillClient(void *destBuf, uint32_t firstSampleFrame, uint32_t numSampleFrames,
                        const void *streamFormat, void *audioStream);

private:
    IOPCIDevice *pci {nullptr};
    IOMemoryMap *bar0 {nullptr};
    IOMemoryMap *bar4 {nullptr};
    volatile uint8_t *hda {nullptr};
    volatile uint8_t *dsp {nullptr};
    IOWorkLoop *workLoop {nullptr};
    IOTimerEventSource *timer {nullptr};

    PlatformFamily platformFamily {kPlatformUnknown};
    uint32_t pciId {0};

    uint32_t numIn {0}, numOut {0}, numBidi {0};
    uint32_t ppOffset {0}, mlOffset {0}, spbOffset {0}, drsmOffset {0};
    uint32_t everRunning {0};        // bitmask: stream descriptors seen with RUN set
    uint32_t tagsSeen[32] {};        // last non-zero stream tag per descriptor
    uint32_t samples {0};
    bool readDsp {false};
    bool ready {false};              // controller decoded + GCAP sane
    uint32_t badReads {0};
    OSArray *logRing {nullptr};      // last log lines, published as "Log" (IOLog may not reach `log show`)

    bool memoryDecoded();
    bool tryInit();
    void logLine(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

    bool inD0();
    uint32_t r32(uint32_t off) { return *reinterpret_cast<volatile uint32_t *>(hda + off); }
    uint16_t r16(uint32_t off) { return *reinterpret_cast<volatile uint16_t *>(hda + off); }
    uint8_t  r8(uint32_t off)  { return hda[off]; }
    uint32_t d32(uint32_t off) { return *reinterpret_cast<volatile uint32_t *>(dsp + off); }

    void walkCapabilities();
    void snapshot(bool verbose);
    void publish();
    static void timerFired(OSObject *owner, IOTimerEventSource *sender);

    // ---- Phase 2: DSP boot (only with -tgldmicboot) ----
    bool bootRequested {false};
    bool keepRunning {false};
    uint32_t bootState {0};           // 0 idle, 1 done ok, 2 failed
    char bootResult[96] {};
    uint32_t savedCgctl {0}, savedPgctl {0}, savedEm2 {0};
    DmaBuffer clBuf, bdlBuf;

    void w32(uint32_t off, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(hda + off) = v; }
    void w16(uint32_t off, uint16_t v) { *reinterpret_cast<volatile uint16_t *>(hda + off) = v; }
    void w8(uint32_t off, uint8_t v)   { hda[off] = v; }
    void dw32(uint32_t off, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(dsp + off) = v; }
    bool pollHda(uint32_t off, uint32_t mask, uint32_t want, uint32_t timeoutUs, uint32_t *last = nullptr);
    bool pollDsp(uint32_t off, uint32_t mask, uint32_t want, uint32_t timeoutUs, uint32_t *last = nullptr);

    uint32_t sdOffset(uint32_t globalIndex) { return 0x80 + 0x20 * globalIndex; }
    bool streamStopReset(uint32_t sd);
    void setNoSnoop(bool on);
    uint16_t savedDevc {0};
    bool devcChanged {false};
    uint32_t capRetries {0};
    bool regSnapTaken {false};
    void snapshotRegs();
    bool prepareLoaderStream(uint32_t globalIndex, uint32_t tag, size_t fwBytes);
    void releaseLoaderStream(uint32_t globalIndex);
    void gating(bool enable);
    uint32_t targetCoreMask() const;
    bool corePowerUp();
    bool coreRun();
    void corePowerDown();
    bool bootDsp();
    bool preallocBuffers();

    // ---- Phase 3: DMIC capture test (only with -tgldmiccap, implies boot) ----
    bool captureRequested {false};
    uint32_t capInIndex {6};
    uint32_t capStreamTag {7};
    uint32_t bootDelaySec {20};      // boot arg tgldmicdelay=<s>, 10..170
    uint32_t captureState {0};        // 0 idle, 1 ok, 2 failed
    char captureResult[96] {};
    DmaBuffer ringBuf, ptBuf, capBdl;
    OSData *capSnap {nullptr};         // 1 s of raw S32_LE 4ch 48 kHz audio, published as "CaptureSnapshot"
    uint32_t chPeak[4] {}, chMeanAbs[4] {};
    uint64_t capBytes {0};
    static constexpr uint32_t kEnvMax = 100;
    uint32_t env[kEnvMax] {};        // per-100 ms mean |x| of ch0
    uint32_t envLen {0};

    bool ipcSend(const uint32_t *msg, uint32_t words, uint32_t *reply = nullptr, uint32_t replyWords = 0);
    void ackDspNotification();
    bool replay(const IpcMsg *list, uint32_t count, const char *phase);
    bool prepareCaptureStream(uint32_t idx, uint32_t tag);
    bool ensureCaptureBuffers();
    void releaseCaptureBuffers();
    bool pipelineStart();
    void pipelineStop();
    bool dspAlive(bool logReason = false);
    void watchdog();
    uint32_t wdPm {0xFFFF}, wdGctl {0}, wdAdspcs {0}, wdTicks {0};
    bool captureTest();

    // ---- Phase 6: feed the DSP microphone into AppleHDA's own input engine ----
    // The native "Internal Microphone" still belongs to AppleHDA. Lilu routes
    // AppleHDAEngine::convertInputSamples; when the internal mic's stream is converted, the hook
    // fills the CoreAudio client buffer directly from the DSP ring and returns. No kernel timer and
    // no writes into the HDA DMA ring are involved.
    bool injectorEnabled {false};
    bool injecting {false};
    uint32_t injectIdx {0};
    uint32_t injectTag {0};
    uint32_t injectFmt {0}, injectFrameBytes {0}, injectChannels {0}, injectBits {0}, injectRate {48000};
    uint32_t injectRingBytes {0}, injectRingFrames {0};
    uint32_t injectGainQ16 {1u << 16}; // tgldmicgain=<percent>, software gain applied on the way in
    uint64_t injectStep {1ULL << 32};   // 48000/engineRate in 32.32
    uint32_t injectNextTry {0};        // samples tick before which a failed start is not retried
    bool injectOwnsPipeline {false}, injectBootedDsp {false}, injectPowerHeld {false};
    // AppleHDA moves its stream descriptors between boots, but the internal mic's IOAudioEngine
    // keeps a fixed tag (the last field of IOAudioEngineGlobalUniqueID). Look it up and prefer the
    // running descriptor with that tag instead of trusting an index.
    uint32_t imicTag {0};              // 0 = not found yet
    uint32_t injectCandidates {0};     // running coupled input descriptors found by the last watch
    uint32_t imicSearchTick {0};       // samples tick before the next engine lookup
    uint32_t imicSafetyFrames {57};    // IOAudioEngineInputSampleOffset of the internal mic
    static constexpr uint32_t kInjectRate = 48000, kInjectSrcFrames = kRingBytes / 16;
    bool findImicTag();
    bool injectorWatch();
    bool injectorStart(uint32_t idx);
    void injectorStop(const char *why);

    // ---- Lilu hook state ----
    static TGLDmic *gInstance;
    static bool hookInstalled;
    bool hookWaitLogged {false};       // one-time "waiting for the hook" message
    bool notImicLogged {false};        // one-time "another input is active" message
    volatile uint32_t hookInFlight {0}; // fast-path reentrancy guard around stop/power-down
    uint64_t hookFills {0}, hookFrames {0};
    uint64_t hookFillsSeen {0};
    // DPIB may be updated in DSP period bursts; the source phase is therefore free-run from
    // mach_absolute_time and resynchronised to DPIB only when the two drift apart.
    bool hookDspInit {false};
    uint64_t hookDspQ {0};             // smoothed DSP write position, 32.32 ring frames
    uint64_t hookDspTime {0};          // mach_absolute_time of hookDspQ
    // The internal microphone's IOAudioStream objects, children of AppleHDAEngineInput. Retained;
    // convertInputSamples runs for every input stream, so the hook only fills these.
    IOService *imicEngine {nullptr};
    IOService *imicStreams[8] {};
    uint32_t imicStreamCount {0};
    void cacheImicStreams(IOService *engine);
    void releaseImicStreams();

    IONotifier *sleepNotifier {nullptr};
    static IOReturn sleepWakeHandler(void *target, void *refCon, UInt32 messageType,
                                     IOService *provider, void *messageArgument, vm_size_t argSize);

    // ---- Runtime / power state ----
    bool serviceMode {false};
    bool streaming {false};
    bool setupDone {false}, triggered {false};
    bool gatingOff {false};
    IOCommandGate *gate {nullptr};
    static IOReturn gatedShutdown(OSObject *o, void *, void *, void *, void *);
    static IOReturn gatedPowerLost(OSObject *o, void *, void *, void *, void *);
    // Power management: TGLDmic is a PM child of the HDA PCI device. While injection is active we
    // hold "on" (state 1), so the HDA driver's idle D3 cannot reset the DSP mid-stream.
    bool pmJoined {false};
    IOService *pmParent {nullptr};    // the HDA driver (AppleHDAController / HDA-Universal / VoodooHDA), else the PCI device
    uint32_t pmSearchTicks {0};
    bool resetLogged {false};
    void joinPowerParent();
    unsigned long pmState {0};
    bool holdPower();                 // request state 1 and wait for D0 + memory decoding
    void releasePower();
    void capFail(const char *what, uint32_t value);
    void fail(const char *what, uint32_t value);
};
