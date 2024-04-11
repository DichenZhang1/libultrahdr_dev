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

#include "ultrahdr/editorhelper.h"
#include "ultrahdr/ultrahdr.h"
#include "ultrahdr/heifr.h"
#include "ultrahdr/jpegr.h"
#include "ultrahdr/jpegrutils.h"
#include "ultrahdr/ultrahdrcommon.h"
#include "ultrahdr/gainmapmath.h"

#include "libheif/api_structs.h"
#include "libheif/heif.h"

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

#define MAX_BUFFER_SIZE 3840 * 2160 * 3 / 2

const int kJobSzInRows = 16;
static_assert(kJobSzInRows > 0 && kJobSzInRows % ultrahdr::kMapDimensionScaleFactor == 0,
              "align job size to kMapDimensionScaleFactor");

namespace ultrahdr {
// returns true if the input file is JPEG, JPEG_R
static bool isJpeg(uint8_t* data, int len) {
  if (data == nullptr || len < 3) {
    return false;
  }
  char brand[4];
  brand[0] = data[0];
  brand[1] = data[1];
  brand[2] = data[2];
  if (data[0] == 0xFF && data[1] == 0xD8 || data[2] == 0xFF) {
    return true;
  }

  return false;
}

// returns true if the input file is AVIF, AVIF_R, AVIF_10_BIT
static bool isAvif(uint8_t* data, int len) {
  if (data == nullptr || len < 12) {
    return false;
  }
  char brand[5];
  brand[0] = data[8];
  brand[1] = data[9];
  brand[2] = data[10];
  brand[3] = data[11];
  brand[4] = 0;
  if (strcmp(brand, "avif") == 0 || strcmp(brand, "avis") == 0) {
      return true;
  }
    return false;
}

// returns true if the input file is HEIC, HEIC_R, HEIC_10_BIT
static bool isHeic(uint8_t* data, int len) {
  if (data == nullptr || len < 12) {
    return false;
  }
  char brand[5];
  brand[0] = data[8];
  brand[1] = data[9];
  brand[2] = data[10];
  brand[3] = data[11];
  brand[4] = 0;
  if (strcmp(brand, "heic") == 0 ||
          strcmp(brand, "heix") == 0 ||
          strcmp(brand, "heim") == 0 ||
          strcmp(brand, "heis") == 0 ||
          strcmp(brand, "mif1") == 0 ||
          strcmp(brand, "hevc") == 0 ||
          strcmp(brand, "hevx") == 0 ||
          strcmp(brand, "hevm") == 0 ||
          strcmp(brand, "hevs") == 0 ||
          strcmp(brand, "msf1") == 0) {
      return true;
  }
    return false;
}

static bool isHeif(uint8_t* data, int len) {
  return (isHeic(data, len) || isAvif(data, len));
}

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
  dest->pixelFormat = ULTRAHDR_PIX_FMT_MONOCHROME;
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

  metadata->version = kGainMapVersion;
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
    size_t height = yuv420_image_ptr->height;

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
            case ULTRAHDR_OUTPUT_HDR_LINEAR_RGB_10BIT: {
              uint16_t r = 0x3ff & static_cast<uint32_t>(rgb_hdr.r * 1023.0f);
              uint16_t g = 0x3ff & static_cast<uint32_t>(rgb_hdr.g * 1023.0f);
              uint16_t b = 0x3ff & static_cast<uint32_t>(rgb_hdr.b * 1023.0f);
              reinterpret_cast<uint16_t*>(dest->data)[                     pixel_idx] = r;
              reinterpret_cast<uint16_t*>(dest->data)[width * height +     pixel_idx] = g;
              reinterpret_cast<uint16_t*>(dest->data)[width * height * 2 + pixel_idx] = b;
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

static struct heif_error writer_write(struct heif_context* ctx, const void* data, size_t size, void* userdata)
{
  MemoryWriter* writer = static_cast<MemoryWriter*>(userdata);
  writer->write(data, size);
  struct heif_error err{heif_error_Ok, heif_suberror_Unspecified, nullptr};
  return err;
}

void UltraHdr::createOutputMemory(int size, void*& dest) {
  if (dest != nullptr) {
    return;
  }
  dest = new uint8_t[size];
  std::shared_ptr<uint8_t[]> dest_data;
  dest_data.reset(reinterpret_cast<uint8_t*>(dest));
  shared_output_data.push_back(dest_data);
}

status_t UltraHdr::addImage(ultrahdr_compressed_struct* image) {
  if (isJpeg((uint8_t*)image->data, image->length)) {
    // JPEG
    // JPEG_R
    ultrahdr_compressed_struct primary_jpeg_image, gainmap_jpeg_image;
    if (status_t err = JpegR::extractPrimaryImageAndGainMap(
            image, &primary_jpeg_image, &gainmap_jpeg_image); err == ULTRAHDR_NO_ERROR) {
      if (sdr_jpeg_img == nullptr) {
        sdr_jpeg_img = std::make_shared<ultrahdr_compressed_struct>();
        sdr_jpeg_img->data = new uint8_t[primary_jpeg_image.length];
        sdr_jpeg_img_data.reset(reinterpret_cast<uint8_t*>(sdr_jpeg_img->data));
        sdr_jpeg_img->length = primary_jpeg_image.length;
        sdr_jpeg_img->colorGamut = image->colorGamut;
        memcpy(sdr_jpeg_img->data, primary_jpeg_image.data, primary_jpeg_image.length);
      }
      if (gain_map_raw_img == nullptr || gain_map_metadata == nullptr) {
        JpegDecoderHelper jpeg_dec_obj_gainmap;
        if (!jpeg_dec_obj_gainmap.decompressImage(gainmap_jpeg_image.data, gainmap_jpeg_image.length)) {
          return ERROR_ULTRAHDR_DECODE_ERROR;
        }
        if (gain_map_raw_img == nullptr) {
          int size = jpeg_dec_obj_gainmap.getDecompressedImageSize();
          gain_map_raw_img = std::make_shared<ultrahdr_uncompressed_struct>();
          gain_map_raw_img->data = new uint8_t[size];
          gain_map_raw_img_data.reset(reinterpret_cast<uint8_t*>(gain_map_raw_img->data));
          memcpy(gain_map_raw_img->data, jpeg_dec_obj_gainmap.getDecompressedImagePtr(), size);
          gain_map_raw_img->width = jpeg_dec_obj_gainmap.getDecompressedImageWidth();
          gain_map_raw_img->height = jpeg_dec_obj_gainmap.getDecompressedImageHeight();
        }
        if (gain_map_metadata == nullptr) {
          gain_map_metadata = std::make_shared<ultrahdr_metadata_struct>();
          if (!getMetadataFromXMP(static_cast<uint8_t*>(jpeg_dec_obj_gainmap.getXMPPtr()),
                  jpeg_dec_obj_gainmap.getXMPSize(), gain_map_metadata.get())) {
            return ERROR_ULTRAHDR_METADATA_ERROR;
          }
        }
        JpegDecoderHelper jpeg_dec_obj_yuv420;
        if (!jpeg_dec_obj_yuv420.decompressImage(
                primary_jpeg_image.data, primary_jpeg_image.length)) {
          return ERROR_ULTRAHDR_DECODE_ERROR;
        }
      }
    } else {
      // normal JPEG
      if (sdr_jpeg_img == nullptr) {
        sdr_jpeg_img = std::make_shared<ultrahdr_compressed_struct>();
        sdr_jpeg_img->data = new uint8_t[image->length];
        sdr_jpeg_img_data.reset(reinterpret_cast<uint8_t*>(sdr_jpeg_img->data));
        sdr_jpeg_img->length = image->length;
        sdr_jpeg_img->colorGamut = image->colorGamut;
        memcpy(sdr_jpeg_img->data, image->data, image->length);
      }
    }
  } else if (isHeif((uint8_t*)image->data, image->length)) {
      //  ULTRAHDR_CODEC_HEIC,
      //  ULTRAHDR_CODEC_AVIF,
      //  ULTRAHDR_CODEC_HEIC_R,
      //  ULTRAHDR_CODEC_AVIF_R,
      //  ULTRAHDR_CODEC_HEIC_10_BIT,
      //  ULTRAHDR_CODEC_AVIF_10_BIT,

      heif_context* ctx = heif_context_alloc();
      heif_context_read_from_memory_without_copy(ctx, image->data, image->length, nullptr);

      // primary image handle
      heif_image_handle* handle;
      heif_image* heif_img;
      heif_context_get_primary_image_handle(ctx, &handle);

      // exif
      if (exif == nullptr) {
        heif_item_id exif_id;
        int n = heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", &exif_id, 1);
        if (n == 1) {
          exif = std::make_shared<ultrahdr_exif_struct>();
          exif->length = heif_image_handle_get_metadata_size(handle, exif_id);
          exif->data = new uint8_t[exif->length];
          exif_data.reset(reinterpret_cast<uint8_t*>(exif->data));
          struct heif_error error = heif_image_handle_get_metadata(handle, exif_id, (uint8_t*) exif->data);
        }
      }

      // primary image
      if (heif_image_handle_get_luma_bits_per_pixel(handle) == 10) {
        heif_decode_image(handle, &heif_img, heif_colorspace_YCbCr, heif_chroma_420, nullptr);
        if (hdr_raw_img == nullptr) {
          hdr_raw_img = std::make_shared<ultrahdr_uncompressed_struct>();
          int width = heif_img->image->get_width();
          int height = heif_img->image->get_height();
          hdr_raw_img_data = std::make_unique<uint8_t[]>(width * height * 3);
          hdr_raw_img->data = hdr_raw_img_data.get();
          hdr_raw_img->chroma_data = (uint8_t*)hdr_raw_img->data + width * height * 2;
          hdr_raw_img->width = width;
          hdr_raw_img->height = height;
          hdr_raw_img->pixelFormat = ULTRAHDR_PIX_FMT_P010;
          hdr_raw_img->colorGamut = image->colorGamut;
          hdr_raw_img->luma_stride = hdr_raw_img->width;
          hdr_raw_img->chroma_stride = hdr_raw_img->luma_stride;
          read_image_as_p010(heif_img, width, height, hdr_raw_img->data);

          return ULTRAHDR_NO_ERROR;
        }
      } else if (heif_image_handle_get_luma_bits_per_pixel(handle) == 8) {
        heif_decode_image(handle, &heif_img, heif_colorspace_YCbCr, heif_chroma_420, nullptr);
        if (sdr_raw_img == nullptr) {
          sdr_raw_img = std::make_shared<ultrahdr_uncompressed_struct>();
          int width = heif_img->image->get_width();
          int height = heif_img->image->get_height();
          sdr_raw_img_data = std::make_unique<uint8_t[]>(width * height * 3 / 2);
          sdr_raw_img->data = sdr_raw_img_data.get();
          sdr_raw_img->chroma_data = (uint8_t*)sdr_raw_img->data + width * height;
          sdr_raw_img->width = width;
          sdr_raw_img->height = height;
          sdr_raw_img->pixelFormat = ULTRAHDR_PIX_FMT_YUV420;
          sdr_raw_img->colorGamut = image->colorGamut;
          read_one_plane(heif_img, heif_channel_Y, width, height, sdr_raw_img->data);
          read_one_plane(heif_img, heif_channel_Cb, (width + 1) / 2, (height + 1) / 2, (uint8_t*)sdr_raw_img->data + width * height);
          read_one_plane(heif_img, heif_channel_Cr, (width + 1) / 2, (height + 1) / 2, (uint8_t*)sdr_raw_img->data + width * height * 5 / 4);
        }

        if (sdr_heif_img == nullptr) {
          sdr_heif_img = std::make_shared<ultrahdr_compressed_struct>();
          sdr_heif_img->data = new uint8_t[image->length];
          sdr_heif_img_data.reset(reinterpret_cast<uint8_t*>(sdr_heif_img->data));
          sdr_heif_img->length = image->length;
          sdr_heif_img->colorGamut = image->colorGamut;
          memcpy(sdr_heif_img->data, image->data, image->length);
        }

      }

      // try if input has gain map
      heif_image_handle* gain_map_image_handle;
      heif_image* gain_map_image;
      if (struct heif_error err = heif_context_get_gain_map_image_handle(ctx, &gain_map_image_handle);
              err.code != heif_error_Ok) {
        // No gain map.
        return ULTRAHDR_NO_ERROR;
      }

      if (gain_map_raw_img == nullptr || gain_map_metadata == nullptr) {
        // gain map
        if (gain_map_raw_img == nullptr) {
          gain_map_raw_img = std::make_shared<ultrahdr_uncompressed_struct>();
          heif_decode_image(gain_map_image_handle, &gain_map_image, heif_colorspace_undefined, heif_chroma_undefined, nullptr);
          int gm_width = gain_map_image->image->get_width();
          int gm_height = gain_map_image->image->get_height();

          gain_map_raw_img_data = std::make_unique<uint8_t[]>(gm_width * gm_height);
          gain_map_raw_img->data = gain_map_raw_img_data.get();
          gain_map_raw_img->width = gm_width;
          gain_map_raw_img->height = gm_height;
          gain_map_raw_img->pixelFormat = ULTRAHDR_PIX_FMT_MONOCHROME;
          read_one_plane(gain_map_image, heif_channel_Y, gm_width, gm_height, gain_map_raw_img->data);
        }

        // gain map metadata
        if (gain_map_metadata == nullptr) {
          GainMapMetadata gmm;
          heif_image_get_gain_map_metadata(ctx, &gmm);
          gain_map_metadata = std::make_shared<ultrahdr_metadata_struct>();
          convert_libheif_metadata_to_libultrahdr_metadata(gmm, *(gain_map_metadata.get()));
        }
      }

      return ULTRAHDR_NO_ERROR;
    } else {
      return ERROR_ULTRAHDR_UNSUPPORTED_FEATURE;
    }

  return ULTRAHDR_NO_ERROR;
}

status_t UltraHdr::addImage(ultrahdr_uncompressed_struct* image) {
  if (image == nullptr && image->data == nullptr) {
    return ERROR_ULTRAHDR_BAD_PTR;
  }

  switch (image->pixelFormat) {
    case ULTRAHDR_PIX_FMT_P010: {
      if (hdr_raw_img == nullptr) {
        hdr_raw_img = std::make_shared<ultrahdr_uncompressed_struct>();
        int size = image->width * image->height * 3;
        hdr_raw_img->width = image->width;
        hdr_raw_img->height = image->height;
        hdr_raw_img->colorGamut = image->colorGamut;
        hdr_raw_img->pixelFormat = image->pixelFormat;
        hdr_raw_img->luma_stride = image->luma_stride;
        hdr_raw_img->chroma_stride = image->chroma_stride;
        hdr_raw_img_data.reset(new uint8_t[size]);
        hdr_raw_img->data = hdr_raw_img_data.get();
        memcpy(hdr_raw_img->data, image->data, size);

        if (hdr_raw_img->luma_stride == 0) hdr_raw_img->luma_stride = hdr_raw_img->width;
        if (!hdr_raw_img->chroma_data) {
          uint16_t* data = reinterpret_cast<uint16_t*>(hdr_raw_img->data);
          hdr_raw_img->chroma_data = data + hdr_raw_img->luma_stride * hdr_raw_img->height;
          hdr_raw_img->chroma_stride = hdr_raw_img->luma_stride;
        }
      }
      return ULTRAHDR_NO_ERROR;
    }
    case ULTRAHDR_PIX_FMT_YUV420: {
      if (sdr_raw_img == nullptr) {
        sdr_raw_img = std::make_shared<ultrahdr_uncompressed_struct>();
        int size = image->width * image->height * 3 / 2;
        sdr_raw_img->width = image->width;
        sdr_raw_img->height = image->height;
        sdr_raw_img->colorGamut = image->colorGamut;
        sdr_raw_img->pixelFormat = image->pixelFormat;
        sdr_raw_img->luma_stride = image->luma_stride;
        sdr_raw_img->chroma_stride = image->chroma_stride;
        sdr_raw_img_data.reset(new uint8_t[size]);
        sdr_raw_img->data = sdr_raw_img_data.get();
        memcpy(sdr_raw_img->data, image->data, size);

        if (sdr_raw_img->luma_stride == 0) sdr_raw_img->luma_stride = sdr_raw_img->width;
        if (!sdr_raw_img->chroma_data) {
          uint8_t* data = reinterpret_cast<uint8_t*>(sdr_raw_img->data);
          sdr_raw_img->chroma_data = data + sdr_raw_img->luma_stride * sdr_raw_img->height;
          sdr_raw_img->chroma_stride = sdr_raw_img->luma_stride >> 1;
        }
      }
      return ULTRAHDR_NO_ERROR;
    }
    default: {
      return ERROR_ULTRAHDR_UNSUPPORTED_FEATURE;
    }
  }
  return ULTRAHDR_NO_ERROR;
}

status_t UltraHdr::addGainMap(ultrahdr_compressed_struct* gainMapImage,
                              ultrahdr_metadata_struct* gainMapMetadata) {
  // TODO: Add logic here.
  return ULTRAHDR_NO_ERROR;
}

status_t UltraHdr::addExif(ultrahdr_exif_struct* in_exif) {
  if (in_exif == nullptr && in_exif->data == nullptr) {
    return ERROR_ULTRAHDR_BAD_PTR;
  }

  if (exif == nullptr) {
    exif = std::make_shared<ultrahdr_exif_struct>();
    exif->length = in_exif->length;
    memcpy(exif->data, in_exif->data, exif->length);
  }

  return ULTRAHDR_NO_ERROR;
}

status_t UltraHdr::getExif(ultrahdr_exif_struct*& dest) {
  if (dest == nullptr) {
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (exif == nullptr) {
    return ERROR_ULTRAHDR_INSUFFICIENT_RESOURCE;
  }

  dest = exif.get();
  return ULTRAHDR_NO_ERROR;
}

status_t UltraHdr::getGainMap(uhdr_uncompressed_ptr& dest) {
  if (dest == nullptr) {
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (gain_map_raw_img == nullptr) {
    return ERROR_ULTRAHDR_INSUFFICIENT_RESOURCE;
  }

  dest = gain_map_raw_img.get();
  return ULTRAHDR_NO_ERROR;
}

status_t UltraHdr::getGainMapMetadata(ultrahdr_metadata_ptr& metadata) {
  if (metadata == nullptr) {
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (gain_map_metadata == nullptr) {
    return ERROR_ULTRAHDR_INSUFFICIENT_RESOURCE;
  }

  metadata = gain_map_metadata.get();
  return ULTRAHDR_NO_ERROR;
}

status_t UltraHdr::convert(ultrahdr_configuration* config, uhdr_compressed_ptr &dest) {
  if (config == nullptr || dest == nullptr) {
    return ERROR_ULTRAHDR_BAD_PTR;
  }

  switch (config->outputCodec) {
    case ULTRAHDR_CODEC_JPEG: {
      if (sdr_jpeg_img != nullptr && config->effects.empty()) {
        dest = sdr_jpeg_img.get();
        return ULTRAHDR_NO_ERROR;
      }

      maybeToneMapRawHdr();

      if (sdr_raw_img != nullptr) {
        ultrahdr_uncompressed_struct after_effects;
        after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
        std::unique_ptr<uint8_t[]> after_effects_data;
        after_effects_data.reset(reinterpret_cast<uint8_t*>(after_effects.data));

        addEffects(sdr_raw_img.get(), config->effects, &after_effects);

        if (after_effects.luma_stride == 0) after_effects.luma_stride = after_effects.width;
        if (!after_effects.chroma_data) {
          uint8_t* data = reinterpret_cast<uint8_t*>(after_effects.data);
          after_effects.chroma_data = data + after_effects.luma_stride * after_effects.height;
          after_effects.chroma_stride = after_effects.luma_stride >> 1;
        }

        // compress 420 image
        JpegEncoderHelper jpeg_enc_obj_yuv420;
        if (!jpeg_enc_obj_yuv420.compressImage(
                reinterpret_cast<uint8_t*>(after_effects.data),
                reinterpret_cast<uint8_t*>(after_effects.chroma_data), after_effects.width,
                after_effects.height, after_effects.luma_stride,
                after_effects.chroma_stride, config->quality,
                nullptr /* icc data */, 0 /* icc length */)) {
          return ERROR_ULTRAHDR_ENCODE_ERROR;
        }

        int size = static_cast<int>(jpeg_enc_obj_yuv420.getCompressedImageSize());

        createOutputMemory(size, dest->data);
        memcpy(dest->data, jpeg_enc_obj_yuv420.getCompressedImagePtr(), size);
        dest->length = size;
        dest->maxLength = size;
        dest->colorGamut = sdr_raw_img->colorGamut;

        return ULTRAHDR_NO_ERROR;
      }

      return ERROR_ULTRAHDR_INSUFFICIENT_RESOURCE;
    }
    case ULTRAHDR_CODEC_JPEG_R: {
      createOutputMemory(MAX_BUFFER_SIZE, dest->data);
      dest->maxLength = MAX_BUFFER_SIZE;
      // JPEG/R encode API-4
      if (gain_map_jpeg_img != nullptr &&
              sdr_jpeg_img != nullptr &&
              gain_map_metadata != nullptr &&
              config->effects.empty()) {
        JpegR encoder;
        ULTRAHDR_CHECK(encoder.encodeJPEGR(
                sdr_jpeg_img.get(), gain_map_jpeg_img.get(), gain_map_metadata.get(), dest));
        return ULTRAHDR_NO_ERROR;
      }

      // JPEG/R encode API-x
      if (sdr_raw_img != nullptr &&
              gain_map_raw_img != nullptr &&
              gain_map_metadata != nullptr) {
        if (config->effects.empty()) {
          JpegR encoder;
          ULTRAHDR_CHECK(encoder.encodeJPEGR(sdr_raw_img.get(), gain_map_raw_img.get(),
                  gain_map_metadata.get(), dest, config->quality, exif.get()));
          return ULTRAHDR_NO_ERROR;
        } else {
          ultrahdr_uncompressed_struct sdr_raw_img_after_effects;
          sdr_raw_img_after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
          std::unique_ptr<uint8_t[]> sdr_raw_img_after_effects_data;
          sdr_raw_img_after_effects_data.reset(reinterpret_cast<uint8_t*>(sdr_raw_img_after_effects.data));
          ultrahdr_uncompressed_struct gain_map_raw_img_after_effects;
          gain_map_raw_img_after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
          std::unique_ptr<uint8_t[]> gain_map_raw_img_after_effects_data;
          gain_map_raw_img_after_effects_data.reset(reinterpret_cast<uint8_t*>(gain_map_raw_img_after_effects.data));
          ULTRAHDR_CHECK(addEffects(sdr_raw_img.get(), config->effects, &sdr_raw_img_after_effects));
          ULTRAHDR_CHECK(addEffects(gain_map_raw_img.get(), config->effects, &gain_map_raw_img_after_effects));
          JpegR encoder;
          ULTRAHDR_CHECK(encoder.encodeJPEGR(&sdr_raw_img_after_effects, &gain_map_raw_img_after_effects,
                  gain_map_metadata.get(), dest, config->quality, exif.get()));
          return ULTRAHDR_NO_ERROR;
        }
      }

      // JPEG/R encode API-2
      if (hdr_raw_img != nullptr &&
              sdr_raw_img != nullptr &&
              sdr_jpeg_img != nullptr &&
              config->effects.empty()) {
        JpegR encoder;
        ULTRAHDR_CHECK(encoder.encodeJPEGR(hdr_raw_img.get(), sdr_raw_img.get(),
                sdr_jpeg_img.get(), config->transferFunction, dest));
        return ULTRAHDR_NO_ERROR;
      }

      // JPEG/R encode API-3
      if (hdr_raw_img != nullptr &&
              sdr_jpeg_img != nullptr &&
              config->effects.empty()) {
        JpegR encoder;
        ULTRAHDR_CHECK(encoder.encodeJPEGR(
                hdr_raw_img.get(), sdr_jpeg_img.get(), config->transferFunction, dest));
        return ULTRAHDR_NO_ERROR;
      }

      // JPEG/R encode API-1
      if (hdr_raw_img != nullptr &&
              sdr_raw_img != nullptr) {
        if (config->effects.empty()) {
          JpegR encoder;
          ULTRAHDR_CHECK(encoder.encodeJPEGR(hdr_raw_img.get(), sdr_raw_img.get(),
                  config->transferFunction, dest, config->quality, exif.get()));
          return ULTRAHDR_NO_ERROR;
        } else {
          gain_map_raw_img = std::make_shared<ultrahdr_uncompressed_struct>();
          gain_map_raw_img->data = new uint8_t[hdr_raw_img->width * hdr_raw_img->height];
          gain_map_raw_img_data.reset(reinterpret_cast<uint8_t*>(gain_map_raw_img->data));
          gain_map_metadata = std::make_shared<ultrahdr_metadata_struct>();

          ULTRAHDR_CHECK(generateGainMap(sdr_raw_img.get(), hdr_raw_img.get(),
                  config->transferFunction, gain_map_metadata.get(), gain_map_raw_img.get()));
          ultrahdr_uncompressed_struct sdr_raw_img_after_effects;
          sdr_raw_img_after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
          std::unique_ptr<uint8_t[]> sdr_raw_img_after_effects_data;
          sdr_raw_img_after_effects_data.reset(reinterpret_cast<uint8_t*>(sdr_raw_img_after_effects.data));
          ultrahdr_uncompressed_struct gain_map_raw_img_after_effects;
          gain_map_raw_img_after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
          std::unique_ptr<uint8_t[]> gain_map_raw_img_after_effects_data;
          gain_map_raw_img_after_effects_data.reset(reinterpret_cast<uint8_t*>(gain_map_raw_img_after_effects.data));
          ULTRAHDR_CHECK(addEffects(sdr_raw_img.get(), config->effects, &sdr_raw_img_after_effects));
          ULTRAHDR_CHECK(addEffects(gain_map_raw_img.get(), config->effects, &gain_map_raw_img_after_effects));
          JpegR encoder;
          ULTRAHDR_CHECK(encoder.encodeJPEGR(&sdr_raw_img_after_effects, &gain_map_raw_img_after_effects,
                  gain_map_metadata.get(), dest, config->quality, exif.get()));
          return ULTRAHDR_NO_ERROR;
        }
      }

      // JPEG/R encode API-0
      if (hdr_raw_img != nullptr) {
        if (config->effects.empty()) {
          JpegR encoder;
          ULTRAHDR_CHECK(encoder.encodeJPEGR(
                  hdr_raw_img.get(), config->transferFunction, dest, config->quality, exif.get()));
          return ULTRAHDR_NO_ERROR;
        } else {
          maybeToneMapRawHdr();
          gain_map_raw_img = std::make_shared<ultrahdr_uncompressed_struct>();
          gain_map_raw_img->data = new uint8_t[hdr_raw_img->width * hdr_raw_img->height];
          gain_map_raw_img_data.reset(reinterpret_cast<uint8_t*>(gain_map_raw_img->data));
          gain_map_metadata = std::make_shared<ultrahdr_metadata_struct>();
          ULTRAHDR_CHECK(generateGainMap(sdr_raw_img.get(), hdr_raw_img.get(),
                  config->transferFunction, gain_map_metadata.get(), gain_map_raw_img.get()));
          ultrahdr_uncompressed_struct sdr_raw_img_after_effects;
          sdr_raw_img_after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
          std::unique_ptr<uint8_t[]> sdr_raw_img_after_effects_data;
          sdr_raw_img_after_effects_data.reset(reinterpret_cast<uint8_t*>(sdr_raw_img_after_effects.data));
          ultrahdr_uncompressed_struct gain_map_raw_img_after_effects;
          gain_map_raw_img_after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
          std::unique_ptr<uint8_t[]> gain_map_raw_img_after_effects_data;
          gain_map_raw_img_after_effects_data.reset(reinterpret_cast<uint8_t*>(gain_map_raw_img_after_effects.data));
          ULTRAHDR_CHECK(addEffects(sdr_raw_img.get(), config->effects, &sdr_raw_img_after_effects));
          ULTRAHDR_CHECK(addEffects(gain_map_raw_img.get(), config->effects, &gain_map_raw_img_after_effects));
          JpegR encoder;
          ULTRAHDR_CHECK(encoder.encodeJPEGR(&sdr_raw_img_after_effects, &gain_map_raw_img_after_effects,
                  gain_map_metadata.get(), dest, config->quality, exif.get()));

          return ULTRAHDR_NO_ERROR;
        }
      }

      return ERROR_ULTRAHDR_INSUFFICIENT_RESOURCE;
    }
    case ULTRAHDR_CODEC_HEIC_R:
    case ULTRAHDR_CODEC_AVIF_R: {
      maybeDecodeJpegSdr();
      maybeToneMapRawHdr();
      createOutputMemory(MAX_BUFFER_SIZE, dest->data);
      dest->maxLength = MAX_BUFFER_SIZE;

      // HEIF/R encode API-x
      if (sdr_raw_img != nullptr &&
              gain_map_raw_img != nullptr &&
              gain_map_metadata != nullptr) {
        if (config->effects.empty()) {
          HeifR encoder;
          ULTRAHDR_CHECK(encoder.encodeHeifWithGainMap(sdr_raw_img.get(),
                                                       gain_map_raw_img.get(),
                                                       gain_map_metadata.get(),
                                                       dest,
                                                       config->quality,
                                                       config->outputCodec,
                                                       exif.get()));
          return ULTRAHDR_NO_ERROR;
        } else {
          ultrahdr_uncompressed_struct sdr_raw_img_after_effects;
          sdr_raw_img_after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
          std::unique_ptr<uint8_t[]> sdr_raw_img_after_effects_data;
          sdr_raw_img_after_effects_data.reset(reinterpret_cast<uint8_t*>(sdr_raw_img_after_effects.data));
          ultrahdr_uncompressed_struct gain_map_raw_img_after_effects;
          gain_map_raw_img_after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
          std::unique_ptr<uint8_t[]> gain_map_raw_img_after_effects_data;
          gain_map_raw_img_after_effects_data.reset(reinterpret_cast<uint8_t*>(gain_map_raw_img_after_effects.data));
          ULTRAHDR_CHECK(addEffects(sdr_raw_img.get(), config->effects, &sdr_raw_img_after_effects));
          ULTRAHDR_CHECK(addEffects(gain_map_raw_img.get(), config->effects, &gain_map_raw_img_after_effects));

          HeifR encoder;
          ULTRAHDR_CHECK(encoder.encodeHeifWithGainMap(&sdr_raw_img_after_effects,
                                                       &gain_map_raw_img_after_effects,
                                                       gain_map_metadata.get(),
                                                       dest,
                                                       config->quality,
                                                       config->outputCodec,
                                                       exif.get()));
          return ULTRAHDR_NO_ERROR;
        }
      }

      // HEIF/R encode API-1
      if (hdr_raw_img != nullptr &&
              sdr_raw_img != nullptr) {
        if (config->effects.empty()) {
          HeifR encoder;
          ULTRAHDR_CHECK(encoder.encodeHeifWithGainMap(hdr_raw_img.get(),
                                                       sdr_raw_img.get(),
                                                       config->transferFunction,
                                                       dest,
                                                       config->quality,
                                                       config->outputCodec,
                                                       exif.get()));
          return ULTRAHDR_NO_ERROR;
        } else {
          gain_map_raw_img = std::make_shared<ultrahdr_uncompressed_struct>();
          gain_map_raw_img->data = new uint8_t[hdr_raw_img->width * hdr_raw_img->height];
          gain_map_raw_img_data.reset(reinterpret_cast<uint8_t*>(gain_map_raw_img->data));
          gain_map_metadata = std::make_shared<ultrahdr_metadata_struct>();

          ULTRAHDR_CHECK(generateGainMap(sdr_raw_img.get(), hdr_raw_img.get(),
                  config->transferFunction, gain_map_metadata.get(), gain_map_raw_img.get()));
          ultrahdr_uncompressed_struct sdr_raw_img_after_effects;
          sdr_raw_img_after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
          std::unique_ptr<uint8_t[]> sdr_raw_img_after_effects_data;
          sdr_raw_img_after_effects_data.reset(reinterpret_cast<uint8_t*>(sdr_raw_img_after_effects.data));
          ultrahdr_uncompressed_struct gain_map_raw_img_after_effects;
          gain_map_raw_img_after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
          std::unique_ptr<uint8_t[]> gain_map_raw_img_after_effects_data;
          gain_map_raw_img_after_effects_data.reset(reinterpret_cast<uint8_t*>(gain_map_raw_img_after_effects.data));
          ULTRAHDR_CHECK(addEffects(sdr_raw_img.get(), config->effects, &sdr_raw_img_after_effects));
          ULTRAHDR_CHECK(addEffects(gain_map_raw_img.get(), config->effects, &gain_map_raw_img_after_effects));
          HeifR encoder;
          ULTRAHDR_CHECK(encoder.encodeHeifWithGainMap(&sdr_raw_img_after_effects,
                                                       &gain_map_raw_img_after_effects,
                                                       gain_map_metadata.get(),
                                                       dest,
                                                       config->quality,
                                                       config->outputCodec,
                                                       exif.get()));
          return ULTRAHDR_NO_ERROR;
        }
      }

      // HEIF/R encode API-0
      if (hdr_raw_img != nullptr) {
        if (config->effects.empty()) {
          HeifR encoder;
          ULTRAHDR_CHECK(encoder.encodeHeifWithGainMap(hdr_raw_img.get(),
                                                       config->transferFunction,
                                                       dest,
                                                       config->quality,
                                                       config->outputCodec,
                                                       exif.get()));
          return ULTRAHDR_NO_ERROR;
        } else {
          maybeToneMapRawHdr();
          gain_map_raw_img = std::make_shared<ultrahdr_uncompressed_struct>();
          gain_map_raw_img->data = new uint8_t[hdr_raw_img->width * hdr_raw_img->height];
          gain_map_raw_img_data.reset(reinterpret_cast<uint8_t*>(gain_map_raw_img->data));
          gain_map_metadata = std::make_shared<ultrahdr_metadata_struct>();

          ULTRAHDR_CHECK(generateGainMap(sdr_raw_img.get(), hdr_raw_img.get(),
                  config->transferFunction, gain_map_metadata.get(), gain_map_raw_img.get()));
          ultrahdr_uncompressed_struct sdr_raw_img_after_effects;
          sdr_raw_img_after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
          std::unique_ptr<uint8_t[]> sdr_raw_img_after_effects_data;
          sdr_raw_img_after_effects_data.reset(reinterpret_cast<uint8_t*>(sdr_raw_img_after_effects.data));
          ultrahdr_uncompressed_struct gain_map_raw_img_after_effects;
          gain_map_raw_img_after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
          std::unique_ptr<uint8_t[]> gain_map_raw_img_after_effects_data;
          gain_map_raw_img_after_effects_data.reset(reinterpret_cast<uint8_t*>(gain_map_raw_img_after_effects.data));
          ULTRAHDR_CHECK(addEffects(sdr_raw_img.get(), config->effects, &sdr_raw_img_after_effects));
          ULTRAHDR_CHECK(addEffects(gain_map_raw_img.get(), config->effects, &gain_map_raw_img_after_effects));
          HeifR encoder;
          ULTRAHDR_CHECK(encoder.encodeHeifWithGainMap(&sdr_raw_img_after_effects,
                                                       &gain_map_raw_img_after_effects,
                                                       gain_map_metadata.get(),
                                                       dest,
                                                       config->quality,
                                                       config->outputCodec,
                                                       exif.get()));

          return ULTRAHDR_NO_ERROR;
        }
      }
      return ULTRAHDR_NO_ERROR;
    }
    case ULTRAHDR_CODEC_HEIC:
    case ULTRAHDR_CODEC_AVIF:{
      maybeToneMapRawHdr();
      maybeDecodeJpegSdr();
      createOutputMemory(MAX_BUFFER_SIZE, dest->data);
      dest->maxLength = MAX_BUFFER_SIZE;

      if (sdr_raw_img != nullptr) {
        ultrahdr_uncompressed_struct after_effects;
        after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
        std::unique_ptr<uint8_t[]> after_effects_data;
        after_effects_data.reset(reinterpret_cast<uint8_t*>(after_effects.data));

        addEffects(sdr_raw_img.get(), config->effects, &after_effects);
        HeifR encoder;
        return encoder.encodeHeifWithGainMap(&after_effects,
                                             nullptr, /* gainmap image ptr */
                                             nullptr, /* gain map metadata */
                                             dest,
                                             config->quality,
                                             config->outputCodec,
                                             exif.get());
      }

      return ERROR_ULTRAHDR_INSUFFICIENT_RESOURCE;
    }
    case ULTRAHDR_CODEC_HEIC_10_BIT:
    case ULTRAHDR_CODEC_AVIF_10_BIT:{
      maybeDecodeJpegSdr();
      if (sdr_raw_img == nullptr || gain_map_raw_img == nullptr || gain_map_metadata == nullptr) {
        return ERROR_ULTRAHDR_INSUFFICIENT_RESOURCE;
      }

      createOutputMemory(MAX_BUFFER_SIZE, dest->data);
      dest->maxLength = MAX_BUFFER_SIZE;

      int size = sdr_raw_img->width * sdr_raw_img->height * 8;
      ultrahdr_uncompressed_struct rgba_temp;
      rgba_temp.data = new uint8_t[size];
      std::unique_ptr<uint8_t[]> rgba_temp_data;
      rgba_temp_data.reset(reinterpret_cast<uint8_t*>(rgba_temp.data));

      if (config->effects.empty()) {
        ULTRAHDR_CHECK(applyGainMap(sdr_raw_img.get(), gain_map_raw_img.get(), gain_map_metadata.get(),
                ULTRAHDR_OUTPUT_HDR_LINEAR_RGB_10BIT, 1000, &rgba_temp));
      }

      ultrahdr_uncompressed_struct sdr_raw_img_after_effects;
      sdr_raw_img_after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
      std::unique_ptr<uint8_t[]> sdr_raw_img_after_effects_data;
      sdr_raw_img_after_effects_data.reset(reinterpret_cast<uint8_t*>(sdr_raw_img_after_effects.data));
      ultrahdr_uncompressed_struct gain_map_raw_img_after_effects;
      gain_map_raw_img_after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
      std::unique_ptr<uint8_t[]> gain_map_raw_img_after_effects_data;
      gain_map_raw_img_after_effects_data.reset(reinterpret_cast<uint8_t*>(gain_map_raw_img_after_effects.data));
      ULTRAHDR_CHECK(addEffects(sdr_raw_img.get(), config->effects, &sdr_raw_img_after_effects));
      ULTRAHDR_CHECK(addEffects(gain_map_raw_img.get(), config->effects, &gain_map_raw_img_after_effects));

      ULTRAHDR_CHECK(applyGainMap(&sdr_raw_img_after_effects, &gain_map_raw_img_after_effects, gain_map_metadata.get(),
                      ULTRAHDR_OUTPUT_HDR_LINEAR_RGB_10BIT, 1000, &rgba_temp));

      heif_context* ctx = heif_context_alloc();
      MemoryWriter writer;
      struct heif_writer w;
      w.writer_api_version = 1;
      w.write = writer_write;

      // get the default encoder
      heif_encoder* encoder;
      if (config->outputCodec == ULTRAHDR_CODEC_HEIC_10_BIT ) {
        heif_context_get_encoder_for_format(ctx, heif_compression_HEVC, &encoder);
      } else if (config->outputCodec == ULTRAHDR_CODEC_AVIF_10_BIT ) {
        heif_context_get_encoder_for_format(ctx, heif_compression_AV1, &encoder);
      } else {
        return ERROR_ULTRAHDR_UNSUPPORTED_FEATURE;
      }

      // set the encoder parameters
      heif_encoder_set_lossy_quality(encoder, config->quality);

      heif_image* image;
      struct heif_image_handle* handle;
      int stride;
      heif_image_create(rgba_temp.width, rgba_temp.height,
              heif_colorspace_RGB, heif_chroma_444, &image);

      fill_new_plane(image, heif_channel_R, rgba_temp.width * 2, rgba_temp.height, rgba_temp.width * 2,
              rgba_temp.data, 10);
      fill_new_plane(image, heif_channel_G, rgba_temp.width * 2, rgba_temp.height, rgba_temp.width * 2,
              (uint16_t*)rgba_temp.data + rgba_temp.width * rgba_temp.height, 10);
      fill_new_plane(image, heif_channel_B, rgba_temp.width * 2, rgba_temp.height, rgba_temp.width * 2,
              (uint16_t*)rgba_temp.data + rgba_temp.width * rgba_temp.height * 2, 10);

      heif_context_encode_image(ctx, image, encoder, nullptr, &handle);

      // add exif
      if (exif != nullptr) {
        heif_context_add_exif_metadata(ctx, handle, exif->data, exif->length);
      }

      heif_encoder_release(encoder);
      heif_context_write(ctx, &w, &writer);
      memcpy(dest->data, writer.data(), writer.size());
      dest->length = writer.size();

      return ULTRAHDR_NO_ERROR;
    }

    default: {
      return ERROR_ULTRAHDR_INVALID_OUTPUT_FORMAT;
    }
  }
  return ULTRAHDR_NO_ERROR;
}

status_t UltraHdr::convert(ultrahdr_configuration* config, uhdr_uncompressed_ptr& dest) {
  if (config == nullptr) {
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (config->outputCodec != ULTRAHDR_CODEC_RAW_PIXELS) {
    return ERROR_ULTRAHDR_INVALID_OUTPUT_FORMAT;
  }

  switch (config->pixelFormat) {
    case ULTRAHDR_PIX_FMT_P010: {
      if (hdr_raw_img != nullptr && config->effects.empty()) {
        dest = hdr_raw_img.get();
        return ULTRAHDR_NO_ERROR;
      }
      return ERROR_ULTRAHDR_INSUFFICIENT_RESOURCE;
    }

    case ULTRAHDR_PIX_FMT_YUV420: {
      maybeToneMapRawHdr();
      maybeDecodeJpegSdr();
      if (sdr_raw_img != nullptr) {
        if (config->effects.empty()) {
          dest = sdr_raw_img.get();
          return ULTRAHDR_NO_ERROR;
        }

        ultrahdr_uncompressed_struct after_effects;
        after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
        std::unique_ptr<uint8_t[]> after_effects_data;
        after_effects_data.reset(reinterpret_cast<uint8_t*>(after_effects.data));
        addEffects(sdr_raw_img.get(), config->effects, &after_effects);
        int size = after_effects.width * after_effects.height * 3 / 2;
        createOutputMemory(size, dest->data);
        dest->width = after_effects.width;
        dest->height = after_effects.height;
        dest->colorGamut = after_effects.colorGamut;
        dest->pixelFormat = after_effects.pixelFormat;
        dest->luma_stride = after_effects.luma_stride;
        dest->chroma_stride = after_effects.chroma_stride;
        memcpy(dest->data, after_effects.data, size);

        return ULTRAHDR_NO_ERROR;
      }
    }

    case ULTRAHDR_PIX_FMT_RGBA8888: {
      if (!config->effects.empty()) {
        return ERROR_ULTRAHDR_UNSUPPORTED_FEATURE;
      }
      if (sdr_jpeg_img != nullptr){
        createOutputMemory(MAX_BUFFER_SIZE, dest->data);
        JpegR decoder;
        return decoder.decodeJPEGR(sdr_jpeg_img.get(), dest, config->maxDisplayBoost, nullptr, ULTRAHDR_OUTPUT_SDR);
      }

      if (sdr_heif_img != nullptr){
        int size = sdr_raw_img->width * sdr_raw_img->height * 4;
        createOutputMemory(size, dest->data);
        HeifR decoder;
        return decoder.decodeHeifWithGainMap(sdr_heif_img.get(), dest, config->maxDisplayBoost, nullptr, ULTRAHDR_OUTPUT_SDR);
      }

      return ERROR_ULTRAHDR_INSUFFICIENT_RESOURCE;
    }

    case ULTRAHDR_PIX_FMT_RGBAF16: {
      if (config->transferFunction != ULTRAHDR_TF_LINEAR) {
        return ERROR_ULTRAHDR_UNSUPPORTED_FEATURE;
      }

      maybeDecodeJpegSdr();

      if (sdr_raw_img == nullptr || gain_map_raw_img == nullptr || gain_map_metadata == nullptr) {
        return ERROR_ULTRAHDR_INSUFFICIENT_RESOURCE;
      }

      int size = sdr_raw_img->width * sdr_raw_img->height * 8;
      createOutputMemory(size, dest->data);

      if (config->effects.empty()) {
        return applyGainMap(sdr_raw_img.get(), gain_map_raw_img.get(), gain_map_metadata.get(),
                ULTRAHDR_OUTPUT_HDR_LINEAR, config->maxDisplayBoost, dest);
      }

      ultrahdr_uncompressed_struct sdr_raw_img_after_effects;
      sdr_raw_img_after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
      std::unique_ptr<uint8_t[]> sdr_raw_img_after_effects_data;
      sdr_raw_img_after_effects_data.reset(reinterpret_cast<uint8_t*>(sdr_raw_img_after_effects.data));
      ultrahdr_uncompressed_struct gain_map_raw_img_after_effects;
      gain_map_raw_img_after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
      std::unique_ptr<uint8_t[]> gain_map_raw_img_after_effects_data;
      gain_map_raw_img_after_effects_data.reset(reinterpret_cast<uint8_t*>(gain_map_raw_img_after_effects.data));
      ULTRAHDR_CHECK(addEffects(sdr_raw_img.get(), config->effects, &sdr_raw_img_after_effects));
      ULTRAHDR_CHECK(addEffects(gain_map_raw_img.get(), config->effects, &gain_map_raw_img_after_effects));

      return applyGainMap(&sdr_raw_img_after_effects, &gain_map_raw_img_after_effects, gain_map_metadata.get(),
                      ULTRAHDR_OUTPUT_HDR_LINEAR, config->maxDisplayBoost, dest);
    }

    case ULTRAHDR_PIX_FMT_RGBA1010102: {
      if (config->transferFunction != ULTRAHDR_TF_HLG &&
              config->transferFunction != ULTRAHDR_TF_PQ) {
        return ERROR_ULTRAHDR_UNSUPPORTED_FEATURE;
      }

      maybeDecodeJpegSdr();

      if (sdr_raw_img == nullptr || gain_map_raw_img == nullptr || gain_map_metadata == nullptr) {
        return ERROR_ULTRAHDR_INSUFFICIENT_RESOURCE;
      }

      int size = sdr_raw_img->width * sdr_raw_img->height * 4;
      createOutputMemory(size, dest->data);

      ultrahdr_output_format output_format = ULTRAHDR_OUTPUT_HDR_PQ;
      if (config->transferFunction == ULTRAHDR_TF_HLG) {
        output_format = ULTRAHDR_OUTPUT_HDR_HLG;
      }

      if (config->effects.empty()) {
        return applyGainMap(sdr_raw_img.get(), gain_map_raw_img.get(), gain_map_metadata.get(),
                output_format, config->maxDisplayBoost, dest);
      }

      ultrahdr_uncompressed_struct sdr_raw_img_after_effects;
      sdr_raw_img_after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
      std::unique_ptr<uint8_t[]> sdr_raw_img_after_effects_data;
      sdr_raw_img_after_effects_data.reset(reinterpret_cast<uint8_t*>(sdr_raw_img_after_effects.data));
      ultrahdr_uncompressed_struct gain_map_raw_img_after_effects;
      gain_map_raw_img_after_effects.data = new uint8_t[MAX_BUFFER_SIZE];
      std::unique_ptr<uint8_t[]> gain_map_raw_img_after_effects_data;
      gain_map_raw_img_after_effects_data.reset(reinterpret_cast<uint8_t*>(gain_map_raw_img_after_effects.data));
      ULTRAHDR_CHECK(addEffects(sdr_raw_img.get(), config->effects, &sdr_raw_img_after_effects));
      ULTRAHDR_CHECK(addEffects(gain_map_raw_img.get(), config->effects, &gain_map_raw_img_after_effects));

      return applyGainMap(&sdr_raw_img_after_effects, &gain_map_raw_img_after_effects, gain_map_metadata.get(),
                      output_format, config->maxDisplayBoost, dest);
    }

    default: {
      return ERROR_ULTRAHDR_UNSUPPORTED_FEATURE;
    }

  }

}

status_t UltraHdr::maybeDecodeJpegSdr() {
  if (sdr_jpeg_img == nullptr) {
    return ULTRAHDR_NO_ERROR;
  }
  if (sdr_raw_img != nullptr && exif != nullptr) {
    return ULTRAHDR_NO_ERROR;
  }

  JpegDecoderHelper jpeg_dec_obj_yuv420;
  if (!jpeg_dec_obj_yuv420.decompressImage(sdr_jpeg_img->data, sdr_jpeg_img->length)) {
    return ERROR_ULTRAHDR_DECODE_ERROR;
  }
  int size = jpeg_dec_obj_yuv420.getDecompressedImageSize();
  sdr_raw_img = std::make_shared<ultrahdr_uncompressed_struct>();
  sdr_raw_img->data = new uint8_t[size];
  sdr_raw_img_data.reset(reinterpret_cast<uint8_t*>(sdr_raw_img->data));
  memcpy(sdr_raw_img->data, jpeg_dec_obj_yuv420.getDecompressedImagePtr(), size);
  sdr_raw_img->width = jpeg_dec_obj_yuv420.getDecompressedImageWidth();
  sdr_raw_img->height = jpeg_dec_obj_yuv420.getDecompressedImageHeight();
  sdr_raw_img->colorGamut = sdr_jpeg_img->colorGamut;
  sdr_raw_img->pixelFormat = ULTRAHDR_PIX_FMT_YUV420;

  if (exif == nullptr) {
    exif = std::make_shared<ultrahdr_exif_struct>();
    exif->data = new uint8_t[jpeg_dec_obj_yuv420.getEXIFSize()];
    exif_data.reset(reinterpret_cast<uint8_t*>(exif->data));
    exif->length = jpeg_dec_obj_yuv420.getEXIFSize();
    memcpy(exif->data, jpeg_dec_obj_yuv420.getEXIFPtr(), jpeg_dec_obj_yuv420.getEXIFSize());
  }

  return ULTRAHDR_NO_ERROR;
}

status_t UltraHdr::maybeToneMapRawHdr() {
  if (sdr_raw_img != nullptr || hdr_raw_img == nullptr) {
    return ULTRAHDR_NO_ERROR;
  }

  ultrahdr_uncompressed_struct p010_image = *(hdr_raw_img.get());
  if (p010_image.luma_stride == 0) p010_image.luma_stride = p010_image.width;
  if (!p010_image.chroma_data) {
    uint16_t* data = reinterpret_cast<uint16_t*>(p010_image.data);
    p010_image.chroma_data = data + p010_image.luma_stride * p010_image.height;
    p010_image.chroma_stride = p010_image.luma_stride;
  }

  int size = p010_image.height * p010_image.width * 3 / 2;
  sdr_raw_img = std::make_shared<ultrahdr_uncompressed_struct>();
  sdr_raw_img->data = new uint8_t[size];
  sdr_raw_img_data.reset(reinterpret_cast<uint8_t*>(sdr_raw_img->data));
  sdr_raw_img->width = p010_image.width;
  sdr_raw_img->height = p010_image.height;
  sdr_raw_img->colorGamut = p010_image.colorGamut;
  sdr_raw_img->pixelFormat = ULTRAHDR_PIX_FMT_YUV420;
  sdr_raw_img->luma_stride = p010_image.luma_stride;
  sdr_raw_img->chroma_stride = p010_image.luma_stride >> 1;
  uint8_t* data = reinterpret_cast<uint8_t*>(sdr_raw_img->data);
  sdr_raw_img->chroma_data = data + sdr_raw_img->luma_stride * sdr_raw_img->height;

  ULTRAHDR_CHECK(toneMap(&p010_image, sdr_raw_img.get()));

  return ULTRAHDR_NO_ERROR;
}



}  // namespace ultrahdr
