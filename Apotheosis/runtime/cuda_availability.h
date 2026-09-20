#ifndef RUNTIME_CUDA_AVAILABILITY_H
#define RUNTIME_CUDA_AVAILABILITY_H

#include <string>

namespace runtime
{
struct CudaRuntimeStatus
{
    bool cudart_loadable = false;
    bool nvinfer_loadable = false;
    bool nvonnxparser_loadable = false;
    bool device_available = false;
    int cuda_runtime_version = 0;
    int device_count = 0;
    std::string failure_reason;

    bool trt_ready() const noexcept
    {
        return cudart_loadable && nvinfer_loadable && nvonnxparser_loadable && device_available;
    }
};

const CudaRuntimeStatus& probe_cuda_runtime();

bool is_tensorrt_available();
}

#endif // RUNTIME_CUDA_AVAILABILITY_H
