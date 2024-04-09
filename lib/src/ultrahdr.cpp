/*
 * Copyright 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef _WIN32
#include <Windows.h>
#include <sysinfoapi.h>
#else
#include <unistd.h>
#endif

#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "ultrahdr/ultrahdr.h"
#include "ultrahdr/ultrahdrcommon.h"
#include "ultrahdr/gainmapmath.h"

int GetCPUCoreCount() {
  int cpuCoreCount = 1;

#if defined(_WIN32)
  SYSTEM_INFO system_info;
  ZeroMemory(&system_info, sizeof(system_info));
  GetSystemInfo(&system_info);
  cpuCoreCount = (size_t)system_info.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
  cpuCoreCount = sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(_SC_NPROCESSORS_CONF)
  cpuCoreCount = sysconf(_SC_NPROCESSORS_CONF);
#else
#error platform-specific implementation for GetCPUCoreCount() missing.
#endif
  if (cpuCoreCount <= 0) cpuCoreCount = 1;
  return cpuCoreCount;
}

const int kJobSzInRows = 16;
static_assert(kJobSzInRows > 0 && kJobSzInRows % ultrahdr::kMapDimensionScaleFactor == 0,
              "align job size to kMapDimensionScaleFactor");

namespace ultrahdr {

class JobQueue {
 public:
  bool dequeueJob(size_t& rowStart, size_t& rowEnd);
  void enqueueJob(size_t rowStart, size_t rowEnd);
  void markQueueForEnd();
  void reset();

 private:
  bool mQueuedAllJobs = false;
  std::deque<std::tuple<size_t, size_t>> mJobs;
  std::mutex mMutex;
  std::condition_variable mCv;
};

bool JobQueue::dequeueJob(size_t& rowStart, size_t& rowEnd) {
  std::unique_lock<std::mutex> lock{mMutex};
  while (true) {
    if (mJobs.empty()) {
      if (mQueuedAllJobs) {
        return false;
      } else {
        mCv.wait_for(lock, std::chrono::milliseconds(100));
      }
    } else {
      auto it = mJobs.begin();
      rowStart = std::get<0>(*it);
      rowEnd = std::get<1>(*it);
      mJobs.erase(it);
      return true;
    }
  }
  return false;
}

void JobQueue::enqueueJob(size_t rowStart, size_t rowEnd) {
  std::unique_lock<std::mutex> lock{mMutex};
  mJobs.push_back(std::make_tuple(rowStart, rowEnd));
  lock.unlock();
  mCv.notify_one();
}

void JobQueue::markQueueForEnd() {
  std::unique_lock<std::mutex> lock{mMutex};
  mQueuedAllJobs = true;
  lock.unlock();
  mCv.notify_all();
}

void JobQueue::reset() {
  std::unique_lock<std::mutex> lock{mMutex};
  mJobs.clear();
  mQueuedAllJobs = false;
}

status_t UltraHdr::generateGainMap(uhdr_uncompressed_ptr yuv420_image_ptr,
                                uhdr_uncompressed_ptr p010_image_ptr,
                                ultrahdr_transfer_function hdr_tf, ultrahdr_metadata_ptr metadata,
                                uhdr_uncompressed_ptr dest, bool sdr_is_601) {
  if (yuv420_image_ptr == nullptr || p010_image_ptr == nullptr || metadata == nullptr ||
      dest == nullptr || yuv420_image_ptr->data == nullptr ||
      yuv420_image_ptr->chroma_data == nullptr || p010_image_ptr->data == nullptr ||
      p010_image_ptr->chroma_data == nullptr) {
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (yuv420_image_ptr->width != p010_image_ptr->width ||
      yuv420_image_ptr->height != p010_image_ptr->height) {
    return ERROR_ULTRAHDR_RESOLUTION_MISMATCH;
  }
  if (yuv420_image_ptr->colorGamut == ULTRAHDR_COLORGAMUT_UNSPECIFIED ||
      p010_image_ptr->colorGamut == ULTRAHDR_COLORGAMUT_UNSPECIFIED) {
    return ERROR_ULTRAHDR_INVALID_COLORGAMUT;
  }

  size_t image_width = yuv420_image_ptr->width;
  size_t image_height = yuv420_image_ptr->height;
  size_t map_width = image_width / kMapDimensionScaleFactor;
  size_t map_height = image_height / kMapDimensionScaleFactor;

  dest->data = new uint8_t[map_width * map_height];
  dest->width = map_width;
  dest->height = map_height;
  dest->colorGamut = ULTRAHDR_COLORGAMUT_UNSPECIFIED;
  dest->luma_stride = map_width;
  dest->chroma_data = nullptr;
  dest->chroma_stride = 0;
  std::unique_ptr<uint8_t[]> map_data;
  map_data.reset(reinterpret_cast<uint8_t*>(dest->data));

  ColorTransformFn hdrInvOetf = nullptr;
  float hdr_white_nits;
  switch (hdr_tf) {
    case ULTRAHDR_TF_LINEAR:
      hdrInvOetf = identityConversion;
      // Note: this will produce clipping if the input exceeds kHlgMaxNits.
      // TODO: TF LINEAR will be deprecated.
      hdr_white_nits = kHlgMaxNits;
      break;
    case ULTRAHDR_TF_HLG:
#if USE_HLG_INVOETF_LUT
      hdrInvOetf = hlgInvOetfLUT;
#else
      hdrInvOetf = hlgInvOetf;
#endif
      hdr_white_nits = kHlgMaxNits;
      break;
    case ULTRAHDR_TF_PQ:
#if USE_PQ_INVOETF_LUT
      hdrInvOetf = pqInvOetfLUT;
#else
      hdrInvOetf = pqInvOetf;
#endif
      hdr_white_nits = kPqMaxNits;
      break;
    default:
      // Should be impossible to hit after input validation.
      return ERROR_ULTRAHDR_INVALID_TRANS_FUNC;
  }

  metadata->maxContentBoost = hdr_white_nits / kSdrWhiteNits;
  metadata->minContentBoost = 1.0f;
  metadata->gamma = 1.0f;
  metadata->offsetSdr = 0.0f;
  metadata->offsetHdr = 0.0f;
  metadata->hdrCapacityMin = 1.0f;
  metadata->hdrCapacityMax = metadata->maxContentBoost;

  float log2MinBoost = log2(metadata->minContentBoost);
  float log2MaxBoost = log2(metadata->maxContentBoost);

  ColorTransformFn hdrGamutConversionFn =
      getHdrConversionFn(yuv420_image_ptr->colorGamut, p010_image_ptr->colorGamut);

  ColorCalculationFn luminanceFn = nullptr;
  ColorTransformFn sdrYuvToRgbFn = nullptr;
  switch (yuv420_image_ptr->colorGamut) {
    case ULTRAHDR_COLORGAMUT_BT709:
      luminanceFn = srgbLuminance;
      sdrYuvToRgbFn = srgbYuvToRgb;
      break;
    case ULTRAHDR_COLORGAMUT_P3:
      luminanceFn = p3Luminance;
      sdrYuvToRgbFn = p3YuvToRgb;
      break;
    case ULTRAHDR_COLORGAMUT_BT2100:
      luminanceFn = bt2100Luminance;
      sdrYuvToRgbFn = bt2100YuvToRgb;
      break;
    case ULTRAHDR_COLORGAMUT_UNSPECIFIED:
      // Should be impossible to hit after input validation.
      return ERROR_ULTRAHDR_INVALID_COLORGAMUT;
  }
  if (sdr_is_601) {
    sdrYuvToRgbFn = p3YuvToRgb;
  }

  ColorTransformFn hdrYuvToRgbFn = nullptr;
  switch (p010_image_ptr->colorGamut) {
    case ULTRAHDR_COLORGAMUT_BT709:
      hdrYuvToRgbFn = srgbYuvToRgb;
      break;
    case ULTRAHDR_COLORGAMUT_P3:
      hdrYuvToRgbFn = p3YuvToRgb;
      break;
    case ULTRAHDR_COLORGAMUT_BT2100:
      hdrYuvToRgbFn = bt2100YuvToRgb;
      break;
    case ULTRAHDR_COLORGAMUT_UNSPECIFIED:
      // Should be impossible to hit after input validation.
      return ERROR_ULTRAHDR_INVALID_COLORGAMUT;
  }

  const int threads = (std::min)(GetCPUCoreCount(), 4);
  size_t rowStep = threads == 1 ? image_height : kJobSzInRows;
  JobQueue jobQueue;

  std::function<void()> generateMap = [yuv420_image_ptr, p010_image_ptr, metadata, dest, hdrInvOetf,
                                       hdrGamutConversionFn, luminanceFn, sdrYuvToRgbFn,
                                       hdrYuvToRgbFn, hdr_white_nits, log2MinBoost, log2MaxBoost,
                                       &jobQueue]() -> void {
    size_t rowStart, rowEnd;
    while (jobQueue.dequeueJob(rowStart, rowEnd)) {
      for (size_t y = rowStart; y < rowEnd; ++y) {
        for (size_t x = 0; x < dest->width; ++x) {
          Color sdr_yuv_gamma = sampleYuv420(yuv420_image_ptr, kMapDimensionScaleFactor, x, y);
          Color sdr_rgb_gamma = sdrYuvToRgbFn(sdr_yuv_gamma);
          // We are assuming the SDR input is always sRGB transfer.
#if USE_SRGB_INVOETF_LUT
          Color sdr_rgb = srgbInvOetfLUT(sdr_rgb_gamma);
#else
          Color sdr_rgb = srgbInvOetf(sdr_rgb_gamma);
#endif
          float sdr_y_nits = luminanceFn(sdr_rgb) * kSdrWhiteNits;

          Color hdr_yuv_gamma = sampleP010(p010_image_ptr, kMapDimensionScaleFactor, x, y);
          Color hdr_rgb_gamma = hdrYuvToRgbFn(hdr_yuv_gamma);
          Color hdr_rgb = hdrInvOetf(hdr_rgb_gamma);
          hdr_rgb = hdrGamutConversionFn(hdr_rgb);
          float hdr_y_nits = luminanceFn(hdr_rgb) * hdr_white_nits;

          size_t pixel_idx = x + y * dest->width;
          reinterpret_cast<uint8_t*>(dest->data)[pixel_idx] =
              encodeGain(sdr_y_nits, hdr_y_nits, metadata, log2MinBoost, log2MaxBoost);
        }
      }
    }
  };

  // generate map
  std::vector<std::thread> workers;
  for (int th = 0; th < threads - 1; th++) {
    workers.push_back(std::thread(generateMap));
  }

  rowStep = (threads == 1 ? image_height : kJobSzInRows) / kMapDimensionScaleFactor;
  for (size_t rowStart = 0; rowStart < map_height;) {
    size_t rowEnd = (std::min)(rowStart + rowStep, map_height);
    jobQueue.enqueueJob(rowStart, rowEnd);
    rowStart = rowEnd;
  }
  jobQueue.markQueueForEnd();
  generateMap();
  std::for_each(workers.begin(), workers.end(), [](std::thread& t) { t.join(); });

  map_data.release();
  return ULTRAHDR_NO_ERROR;
}

status_t UltraHdr::applyGainMap(uhdr_uncompressed_ptr yuv420_image_ptr,
                             uhdr_uncompressed_ptr gainmap_image_ptr, ultrahdr_metadata_ptr metadata,
                             ultrahdr_output_format output_format, float max_display_boost,
                             uhdr_uncompressed_ptr dest) {
  if (yuv420_image_ptr == nullptr || gainmap_image_ptr == nullptr || metadata == nullptr ||
      dest == nullptr || yuv420_image_ptr->data == nullptr ||
      yuv420_image_ptr->chroma_data == nullptr || gainmap_image_ptr->data == nullptr) {
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (metadata->version.compare(kGainMapVersion)) {
    ALOGE("Unsupported metadata version: %s", metadata->version.c_str());
    return ERROR_ULTRAHDR_BAD_METADATA;
  }
  if (metadata->gamma != 1.0f) {
    ALOGE("Unsupported metadata gamma: %f", metadata->gamma);
    return ERROR_ULTRAHDR_BAD_METADATA;
  }
  if (metadata->offsetSdr != 0.0f || metadata->offsetHdr != 0.0f) {
    ALOGE("Unsupported metadata offset sdr, hdr: %f, %f", metadata->offsetSdr, metadata->offsetHdr);
    return ERROR_ULTRAHDR_BAD_METADATA;
  }
  if (metadata->hdrCapacityMin != metadata->minContentBoost ||
      metadata->hdrCapacityMax != metadata->maxContentBoost) {
    ALOGE("Unsupported metadata hdr capacity min, max: %f, %f", metadata->hdrCapacityMin,
          metadata->hdrCapacityMax);
    return ERROR_ULTRAHDR_BAD_METADATA;
  }

  if (yuv420_image_ptr->width % gainmap_image_ptr->width != 0 ||
      yuv420_image_ptr->height % gainmap_image_ptr->height != 0) {
    ALOGE(
        "gain map dimensions scale factor value is not an integer, primary image resolution is "
        "%zux%zu, received gain map resolution is %zux%zu",
        yuv420_image_ptr->width, yuv420_image_ptr->height, gainmap_image_ptr->width,
        gainmap_image_ptr->height);
    return ERROR_ULTRAHDR_UNSUPPORTED_MAP_SCALE_FACTOR;
  }

  if (yuv420_image_ptr->width * gainmap_image_ptr->height !=
      yuv420_image_ptr->height * gainmap_image_ptr->width) {
    ALOGE(
        "gain map dimensions scale factor values for height and width are different, \n primary "
        "image resolution is %zux%zu, received gain map resolution is %zux%zu",
        yuv420_image_ptr->width, yuv420_image_ptr->height, gainmap_image_ptr->width,
        gainmap_image_ptr->height);
    return ERROR_ULTRAHDR_UNSUPPORTED_MAP_SCALE_FACTOR;
  }
  // TODO: Currently map_scale_factor is of type size_t, but it could be changed to a float
  // later.
  size_t map_scale_factor = yuv420_image_ptr->width / gainmap_image_ptr->width;

  dest->width = yuv420_image_ptr->width;
  dest->height = yuv420_image_ptr->height;
  dest->colorGamut = yuv420_image_ptr->colorGamut;
  ShepardsIDW idwTable(map_scale_factor);
  float display_boost = (std::min)(max_display_boost, metadata->maxContentBoost);
  GainLUT gainLUT(metadata, display_boost);

  JobQueue jobQueue;
  std::function<void()> applyRecMap = [yuv420_image_ptr, gainmap_image_ptr, dest, &jobQueue,
                                       &idwTable, output_format, &gainLUT, display_boost,
                                       map_scale_factor, &metadata]() -> void {
    size_t width = yuv420_image_ptr->width;

    size_t rowStart, rowEnd;
    while (jobQueue.dequeueJob(rowStart, rowEnd)) {
      for (size_t y = rowStart; y < rowEnd; ++y) {
        for (size_t x = 0; x < width; ++x) {
          Color yuv_gamma_sdr = getYuv420Pixel(yuv420_image_ptr, x, y);
          // Assuming the sdr image is a decoded JPEG, we should always use Rec.601 YUV coefficients
          Color rgb_gamma_sdr = p3YuvToRgb(yuv_gamma_sdr);
          // We are assuming the SDR base image is always sRGB transfer.
#if USE_SRGB_INVOETF_LUT
          Color rgb_sdr = srgbInvOetfLUT(rgb_gamma_sdr);
#else
          Color rgb_sdr = srgbInvOetf(rgb_gamma_sdr);
#endif
          float gain;
          // TODO: If map_scale_factor is guaranteed to be an integer, then remove the following.
          if (map_scale_factor != floorf(map_scale_factor)) {
            gain = sampleMap(gainmap_image_ptr, map_scale_factor, x, y);
          } else {
            gain = sampleMap(gainmap_image_ptr, map_scale_factor, x, y, idwTable);
          }

#if USE_APPLY_GAIN_LUT
          Color rgb_hdr = applyGainLUT(rgb_sdr, gain, gainLUT);
#else
          Color rgb_hdr = applyGain(rgb_sdr, gain, metadata, display_boost);
#endif
          rgb_hdr = rgb_hdr / display_boost;
          size_t pixel_idx = x + y * width;

          switch (output_format) {
            case ULTRAHDR_OUTPUT_HDR_LINEAR: {
              uint64_t rgba_f16 = colorToRgbaF16(rgb_hdr);
              reinterpret_cast<uint64_t*>(dest->data)[pixel_idx] = rgba_f16;
              break;
            }
            case ULTRAHDR_OUTPUT_HDR_HLG: {
#if USE_HLG_OETF_LUT
              ColorTransformFn hdrOetf = hlgOetfLUT;
#else
              ColorTransformFn hdrOetf = hlgOetf;
#endif
              Color rgb_gamma_hdr = hdrOetf(rgb_hdr);
              uint32_t rgba_1010102 = colorToRgba1010102(rgb_gamma_hdr);
              reinterpret_cast<uint32_t*>(dest->data)[pixel_idx] = rgba_1010102;
              break;
            }
            case ULTRAHDR_OUTPUT_HDR_PQ: {
#if USE_PQ_OETF_LUT
              ColorTransformFn hdrOetf = pqOetfLUT;
#else
              ColorTransformFn hdrOetf = pqOetf;
#endif
              Color rgb_gamma_hdr = hdrOetf(rgb_hdr);
              uint32_t rgba_1010102 = colorToRgba1010102(rgb_gamma_hdr);
              reinterpret_cast<uint32_t*>(dest->data)[pixel_idx] = rgba_1010102;
              break;
            }
            default: {
            }
              // Should be impossible to hit after input validation.
          }
        }
      }
    }
  };

  const int threads = (std::min)(GetCPUCoreCount(), 4);
  std::vector<std::thread> workers;
  for (int th = 0; th < threads - 1; th++) {
    workers.push_back(std::thread(applyRecMap));
  }
  const int rowStep = threads == 1 ? yuv420_image_ptr->height : map_scale_factor;
  for (size_t rowStart = 0; rowStart < yuv420_image_ptr->height;) {
    int rowEnd = (std::min)(rowStart + rowStep, yuv420_image_ptr->height);
    jobQueue.enqueueJob(rowStart, rowEnd);
    rowStart = rowEnd;
  }
  jobQueue.markQueueForEnd();
  applyRecMap();
  std::for_each(workers.begin(), workers.end(), [](std::thread& t) { t.join(); });
  return ULTRAHDR_NO_ERROR;
}

status_t UltraHdr::toneMap(uhdr_uncompressed_ptr src, uhdr_uncompressed_ptr dest) {
  if (src == nullptr || dest == nullptr) {
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (src->width != dest->width || src->height != dest->height) {
    return ERROR_ULTRAHDR_RESOLUTION_MISMATCH;
  }
  uint16_t* src_y_data = reinterpret_cast<uint16_t*>(src->data);
  uint8_t* dst_y_data = reinterpret_cast<uint8_t*>(dest->data);
  for (size_t y = 0; y < src->height; ++y) {
    uint16_t* src_y_row = src_y_data + y * src->luma_stride;
    uint8_t* dst_y_row = dst_y_data + y * dest->luma_stride;
    for (size_t x = 0; x < src->width; ++x) {
      uint16_t y_uint = src_y_row[x] >> 6;
      dst_y_row[x] = static_cast<uint8_t>((y_uint >> 2) & 0xff);
    }
    if (dest->width != dest->luma_stride) {
      memset(dst_y_row + dest->width, 0, dest->luma_stride - dest->width);
    }
  }
  uint16_t* src_uv_data = reinterpret_cast<uint16_t*>(src->chroma_data);
  uint8_t* dst_u_data = reinterpret_cast<uint8_t*>(dest->chroma_data);
  size_t dst_v_offset = (dest->chroma_stride * dest->height / 2);
  uint8_t* dst_v_data = dst_u_data + dst_v_offset;
  for (size_t y = 0; y < src->height / 2; ++y) {
    uint16_t* src_uv_row = src_uv_data + y * src->chroma_stride;
    uint8_t* dst_u_row = dst_u_data + y * dest->chroma_stride;
    uint8_t* dst_v_row = dst_v_data + y * dest->chroma_stride;
    for (size_t x = 0; x < src->width / 2; ++x) {
      uint16_t u_uint = src_uv_row[x << 1] >> 6;
      uint16_t v_uint = src_uv_row[(x << 1) + 1] >> 6;
      dst_u_row[x] = static_cast<uint8_t>((u_uint >> 2) & 0xff);
      dst_v_row[x] = static_cast<uint8_t>((v_uint >> 2) & 0xff);
    }
    if (dest->width / 2 != dest->chroma_stride) {
      memset(dst_u_row + dest->width / 2, 0, dest->chroma_stride - dest->width / 2);
      memset(dst_v_row + dest->width / 2, 0, dest->chroma_stride - dest->width / 2);
    }
  }
  dest->colorGamut = src->colorGamut;
  return ULTRAHDR_NO_ERROR;
}

}  // namespace ultrahdr
