#include <mach/mach_types.h>

// libkmod's _start/_stop wrappers forward through _realmain/_antimain, which is how a kext runs
// C++ code at load time. Lilu's plugin_start.cpp provides these entry points; they register the
// IOAudioStream::readInputSamples route before IOAudioFamily is used.
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
extern kern_return_t TGLDmic_kern_start(kmod_info_t *ki, void *data);
extern kern_return_t TGLDmic_kern_stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(com.labrouss.TGLDmic, "0.6.4", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = TGLDmic_kern_start;
__private_extern__ kmod_stop_func_t *_antimain = TGLDmic_kern_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
