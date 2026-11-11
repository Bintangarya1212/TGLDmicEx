// Lilu integration.
//
// The software injector replaces the samples AppleHDA hands to CoreAudio. The hook target is
// AppleHDAEngine::convertInputSamples: for the internal microphone TGLDmic fills the client buffer
// directly from the DSP ring and returns without invoking the original converter, which would only
// read the silent (unconnected analog mic) DMA path. Other input streams go to the original.
//
// Registration is event-driven: TGLDmic only registers a KextInfo listener for AppleHDA and routes
// the function when Lilu notifies that AppleHDA has loaded. It never attempts to route at startup,
// where AppleHDA may not exist yet. KextInfo::Loaded also covers an already-loaded AppleHDA.
#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <IOKit/audio/IOAudioStream.h>
#include <mach/kmod.h>
#include "TGLDmic.hpp"

TGLDmicHookStatus tgldmicHookStatus {};

static const char *bootargOff[] {
    "-tgldmicoff"
};

static const char *bootargDebug[] {
    "-tgldmicdbg"
};

static const char *bootargBeta[] {
    "-tgldmicbeta"
};

static constexpr const char *kConvertInputSymbol =
    "__ZN14AppleHDAEngine19convertInputSamplesEPKvPvjjPK20_IOAudioStreamFormatP13IOAudioStream";

static const char *appleHDAPaths[] {
    "/System/Library/Extensions/AppleHDA.kext/Contents/MacOS/AppleHDA"
};

static KernelPatcher::KextInfo appleHDAInfo {
    "com.apple.driver.AppleHDA", appleHDAPaths, arrsize(appleHDAPaths), {true}, {}, KernelPatcher::KextInfo::Unloaded
};

static mach_vm_address_t orgConvertInputSamples {0};

static IOReturn convertInputSamplesHook(void *engine, const void *sampleBuf, void *destBuf,
                                        UInt32 firstSampleFrame, UInt32 numSampleFrames,
                                        const IOAudioStreamFormat *streamFormat,
                                        IOAudioStream *audioStream) {
    TGLDmic *dev = TGLDmic::instance();
    // Internal microphone: TGLDmic fills the client buffer itself; do not call the original.
    if (dev != nullptr && audioStream != nullptr && numSampleFrames != 0 &&
        dev->hookFillClient(destBuf, firstSampleFrame, numSampleFrames, streamFormat, audioStream))
        return kIOReturnSuccess;
    if (orgConvertInputSamples != 0)
        return FunctionCast(convertInputSamplesHook, orgConvertInputSamples)(engine, sampleBuf, destBuf,
                                                                             firstSampleFrame, numSampleFrames,
                                                                             streamFormat, audioStream);
    return kIOReturnUnsupported;
}

static void installHook(KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    if (TGLDmic::hookIsInstalled()) return;
    KernelPatcher::RouteRequest request(kConvertInputSymbol, convertInputSamplesHook, orgConvertInputSamples);
    bool ok = patcher.routeMultiple(id, &request, 1, address, size);
    patcher.clearError();
    TGLDmic::setHookInstalled(ok);
    tgldmicHookStatus.routeOk = ok ? 1 : 2;
    IOLog("TGLDmic: Lilu hook AppleHDAEngine::convertInputSamples %s (orig %p)\n",
          ok ? "installed" : "FAILED", reinterpret_cast<void *>(orgConvertInputSamples));
}

static void appleHDALoaded(void *, KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    tgldmicHookStatus.kextCb++;
    installHook(patcher, id, address, size);
}

static void pluginStart() {
    tgldmicHookStatus.started = 1;
    bool enabled = checkKernelArgument("-tgldmiclilu") || checkKernelArgument("tgldmiclilu") ||
                   checkKernelArgument("-tgldmicinject") || checkKernelArgument("tgldmicinject");
    if (!enabled) {
        tgldmicHookStatus.arg = 0;
        IOLog("TGLDmic: Lilu hook disabled (boot without -tgldmiclilu/-tgldmicinject)\n");
        return;
    }
    tgldmicHookStatus.arg = 1;
    // Only register the AppleHDA load event. Lilu routes nothing here: it calls appleHDALoaded
    // with id/address/size when AppleHDA is loaded (or immediately if it is already loaded).
    LiluAPI::Error kerr = lilu.onKextLoad(&appleHDAInfo, 1, appleHDALoaded, nullptr);
    tgldmicHookStatus.onKextErr = static_cast<uint32_t>(kerr) + 1;
    IOLog("TGLDmic: Lilu onKextLoad(AppleHDA) registration: %d\n", static_cast<int>(kerr));
}

// Custom kmod entry (LILU_CUSTOM_KMOD_INIT): the standard plugin_start kern_start is not compiled,
// so the Lilu handshake is done here with diagnostics for every stage.
extern "C" kern_return_t TGLDmic_kern_start(kmod_info_t *, void *) {
    tgldmicHookStatus.kmodEntry = 1;
    LiluAPI::Error err = lilu.requestAccess();
    tgldmicHookStatus.reqAccess = static_cast<uint32_t>(err) + 1;
    if (err == LiluAPI::Error::NoError) {
        bool debug = false;
        err = lilu.shouldLoad(xStringify(PRODUCT_NAME),
                              parseModuleVersion(xStringify(MODULE_VERSION)),
                              LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
                              bootargOff, arrsize(bootargOff),
                              bootargDebug, arrsize(bootargDebug),
                              bootargBeta, arrsize(bootargBeta),
                              KernelVersion::Catalina, KernelVersion::Tahoe, debug);
        tgldmicHookStatus.shouldLoad = static_cast<uint32_t>(err) + 1;
        if (err == LiluAPI::Error::NoError) pluginStart();
        lilu.releaseAccess();
    }
    return KERN_SUCCESS;
}

extern "C" kern_return_t TGLDmic_kern_stop(kmod_info_t *, void *) {
    return KERN_FAILURE;
}

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::Catalina,
    KernelVersion::Tahoe,
    pluginStart
};
