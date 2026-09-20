#include "gpu_jpeg_decoder.h"

#include <algorithm>
#include <iostream>

namespace capture {

GpuJpegDecoder::GpuJpegDecoder() = default;

GpuJpegDecoder::~GpuJpegDecoder()
{
    cleanup();
}

void GpuJpegDecoder::cleanup()
{
    cleanupRoi();

    if (state_)
    {
        nvjpegJpegStateDestroy(state_);
        state_ = nullptr;
    }
    if (handle_)
    {
        nvjpegDestroy(handle_);
        handle_ = nullptr;
    }
    initialized_ = false;
}

void GpuJpegDecoder::cleanupRoi()
{
    if (roi_params_) { nvjpegDecodeParamsDestroy(roi_params_); roi_params_ = nullptr; }
    for (int i = 0; i < ROI_RING; ++i)
    {
        if (roi_event_[i])  { cudaEventDestroy(roi_event_[i]);           roi_event_[i]  = nullptr; }
        if (roi_stream_[i]) { nvjpegJpegStreamDestroy(roi_stream_[i]);   roi_stream_[i] = nullptr; }
        if (roi_device_[i]) { nvjpegBufferDeviceDestroy(roi_device_[i]); roi_device_[i] = nullptr; }
        if (roi_pinned_[i]) { nvjpegBufferPinnedDestroy(roi_pinned_[i]); roi_pinned_[i] = nullptr; }
        if (roi_state_[i])  { nvjpegJpegStateDestroy(roi_state_[i]);     roi_state_[i]  = nullptr; }
    }
    if (roi_decoder_) { nvjpegDecoderDestroy(roi_decoder_); roi_decoder_ = nullptr; }
    roi_slot_ = 0;
    roi_initialized_ = false;
}

bool GpuJpegDecoder::initRoi()
{
    if (roi_initialized_) return true;
    if (!initialized_) return false;

    nvjpegStatus_t st = nvjpegDecoderCreate(handle_, NVJPEG_BACKEND_DEFAULT, &roi_decoder_);
    if (st != NVJPEG_STATUS_SUCCESS) { cleanupRoi(); return false; }

    st = nvjpegDecodeParamsCreate(handle_, &roi_params_);
    if (st != NVJPEG_STATUS_SUCCESS) { cleanupRoi(); return false; }
    nvjpegDecodeParamsSetOutputFormat(roi_params_, NVJPEG_OUTPUT_BGRI);

    for (int i = 0; i < ROI_RING; ++i)
    {
        st = nvjpegDecoderStateCreate(handle_, roi_decoder_, &roi_state_[i]);
        if (st != NVJPEG_STATUS_SUCCESS) { cleanupRoi(); return false; }

        st = nvjpegBufferPinnedCreate(handle_, nullptr, &roi_pinned_[i]);
        if (st != NVJPEG_STATUS_SUCCESS) { cleanupRoi(); return false; }

        st = nvjpegBufferDeviceCreate(handle_, nullptr, &roi_device_[i]);
        if (st != NVJPEG_STATUS_SUCCESS) { cleanupRoi(); return false; }

        st = nvjpegJpegStreamCreate(handle_, &roi_stream_[i]);
        if (st != NVJPEG_STATUS_SUCCESS) { cleanupRoi(); return false; }

        nvjpegStateAttachPinnedBuffer(roi_state_[i], roi_pinned_[i]);
        nvjpegStateAttachDeviceBuffer(roi_state_[i], roi_device_[i]);

        if (cudaEventCreateWithFlags(&roi_event_[i], cudaEventDisableTiming) != cudaSuccess)
            roi_event_[i] = nullptr;
    }

    roi_slot_ = 0;
    roi_initialized_ = true;
    return true;
}

bool GpuJpegDecoder::decodeCropped(
    const uint8_t* data,
    size_t size,
    int cropW,
    int cropH,
    GpuImage& dst,
    cudaStream_t stream)
{
    if (!initialized_ || !data || size == 0) return false;
    if (cropW <= 0 || cropH <= 0) return false;
    if (!initRoi()) return false;

    const size_t slot = roi_slot_;
    roi_slot_ = (roi_slot_ + 1) % ROI_RING;
    nvjpegJpegState_t state = roi_state_[slot];
    nvjpegJpegStream_t jpegStream = roi_stream_[slot];

    if (roi_event_[slot])
        cudaEventSynchronize(roi_event_[slot]);

    nvjpegStatus_t st = nvjpegJpegStreamParse(handle_, data, size, 0, 0, jpegStream);
    if (st != NVJPEG_STATUS_SUCCESS) return false;

    unsigned int fw = 0;
    unsigned int fh = 0;
    if (nvjpegJpegStreamGetFrameDimensions(jpegStream, &fw, &fh) != NVJPEG_STATUS_SUCCESS)
        return false;
    const int W = static_cast<int>(fw);
    const int H = static_cast<int>(fh);
    if (W <= 0 || H <= 0) return false;

    const int roiW = std::min(cropW, W);
    const int roiH = std::min(cropH, H);
    const int offX = std::max(0, (W - roiW) / 2) & ~1;
    const int offY = std::max(0, (H - roiH) / 2) & ~1;

    if (nvjpegDecodeParamsSetROI(roi_params_, offX, offY, roiW, roiH) != NVJPEG_STATUS_SUCCESS)
        return false;

    if (!dst.create(roiH, roiW, 3))
        return false;

    nvjpegImage_t out{};
    out.channel[0] = dst.data();
    out.pitch[0] = static_cast<unsigned int>(dst.step());

    st = nvjpegDecodeJpegHost(handle_, roi_decoder_, state, roi_params_, jpegStream);
    if (st != NVJPEG_STATUS_SUCCESS) return false;

    st = nvjpegDecodeJpegTransferToDevice(handle_, roi_decoder_, state, jpegStream, stream);
    if (st != NVJPEG_STATUS_SUCCESS) return false;

    st = nvjpegDecodeJpegDevice(handle_, roi_decoder_, state, &out, stream);
    if (st != NVJPEG_STATUS_SUCCESS) return false;

    if (roi_event_[slot])
        cudaEventRecord(roi_event_[slot], stream);

    return true;
}

bool GpuJpegDecoder::init()
{
    if (initialized_) return true;

    nvjpegStatus_t st = nvjpegCreateSimple(&handle_);
    if (st != NVJPEG_STATUS_SUCCESS)
    {
        std::cerr << "[GpuJpegDecoder] nvjpegCreateSimple failed: " << st << std::endl;
        cleanup();
        return false;
    }

    st = nvjpegJpegStateCreate(handle_, &state_);
    if (st != NVJPEG_STATUS_SUCCESS)
    {
        std::cerr << "[GpuJpegDecoder] nvjpegJpegStateCreate failed: " << st << std::endl;
        cleanup();
        return false;
    }

    initialized_ = true;
    return true;
}

bool GpuJpegDecoder::decode(
    const uint8_t* data,
    size_t size,
    GpuImage& dst,
    cudaStream_t stream)
{
    if (!initialized_ || !data || size == 0) return false;

    int nComponents = 0;
    nvjpegChromaSubsampling_t subsampling;
    int widths[NVJPEG_MAX_COMPONENT] = { 0 };
    int heights[NVJPEG_MAX_COMPONENT] = { 0 };

    nvjpegStatus_t st = nvjpegGetImageInfo(
        handle_, data, size, &nComponents, &subsampling, widths, heights);
    if (st != NVJPEG_STATUS_SUCCESS)
        return false;

    const int W = widths[0];
    const int H = heights[0];
    if (W <= 0 || H <= 0) return false;

    if (!dst.create(H, W, 3))
        return false;

    nvjpegImage_t out{};
    out.channel[0] = dst.data();
    out.pitch[0] = static_cast<unsigned int>(dst.step());

    st = nvjpegDecode(
        handle_, state_, data, size,
        NVJPEG_OUTPUT_BGRI, &out, stream);

    return st == NVJPEG_STATUS_SUCCESS;
}

}
