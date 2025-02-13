#include "Decoder.h"

#if DORADO_GPU_BUILD && !defined(__APPLE__)
#include "CUDADecoder.h"
#endif

#include "CPUDecoder.h"
#include "basecall/CRFModelConfig.h"

#include <c10/core/Device.h>

namespace dorado::basecall::decode {

std::unique_ptr<Decoder> create_decoder(c10::Device device, const CRFModelConfig& config) {
#if DORADO_GPU_BUILD && !defined(__APPLE__)
    if (device.is_cuda()) {
        return std::make_unique<decode::CUDADecoder>(config.clamp ? 5.f : 0.f);
    }
#else
    (void)config;  // unused in other build types
#endif
    if (device.is_cpu()) {
        return std::make_unique<decode::CPUDecoder>();
    }

    throw std::runtime_error("Unsupported device type for decoder creation: " + device.str());
}

}  // namespace dorado::basecall::decode
