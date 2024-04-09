/*
 * Copyright 2023 The Android Open Source Project
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

#ifndef ULTRAHDR_ULTRAHDR_H
#define ULTRAHDR_ULTRAHDR_H

#include <string>

namespace ultrahdr {
#define ULTRAHDR_CHECK(x)             \
  {                                   \
    status_t status = (x);            \
    if ((status) != ULTRAHDR_NO_ERROR) { \
      return status;                  \
    }                                 \
  }

// Color gamuts for image data
typedef enum {
  ULTRAHDR_COLORGAMUT_UNSPECIFIED = -1,
  ULTRAHDR_COLORGAMUT_BT709,
  ULTRAHDR_COLORGAMUT_P3,
  ULTRAHDR_COLORGAMUT_BT2100,
  ULTRAHDR_COLORGAMUT_MAX = ULTRAHDR_COLORGAMUT_BT2100,
} ultrahdr_color_gamut;

// Transfer functions for image data
// TODO: TF LINEAR is deprecated, remove this enum and the code surrounding it.
typedef enum {
  ULTRAHDR_TF_UNSPECIFIED = -1,
  ULTRAHDR_TF_LINEAR = 0,
  ULTRAHDR_TF_HLG = 1,
  ULTRAHDR_TF_PQ = 2,
  ULTRAHDR_TF_SRGB = 3,
  ULTRAHDR_TF_MAX = ULTRAHDR_TF_SRGB,
} ultrahdr_transfer_function;

// Target output formats for decoder
typedef enum {
  ULTRAHDR_OUTPUT_UNSPECIFIED = -1,
  ULTRAHDR_OUTPUT_SDR,         // SDR in RGBA_8888 color format
  ULTRAHDR_OUTPUT_HDR_LINEAR,  // HDR in F16 color format (linear)
  ULTRAHDR_OUTPUT_HDR_PQ,      // HDR in RGBA_1010102 color format (PQ transfer function)
  ULTRAHDR_OUTPUT_HDR_HLG,     // HDR in RGBA_1010102 color format (HLG transfer function)
  ULTRAHDR_OUTPUT_MAX = ULTRAHDR_OUTPUT_HDR_HLG,
} ultrahdr_output_format;

// Supported pixel format
typedef enum {
  ULTRAHDR_PIX_FMT_UNSPECIFIED = -1,
  ULTRAHDR_PIX_FMT_P010,
  ULTRAHDR_PIX_FMT_YUV420,
  ULTRAHDR_PIX_FMT_MONOCHROME,
  ULTRAHDR_PIX_FMT_RGBA8888,
  ULTRAHDR_PIX_FMT_RGBAF16,
  ULTRAHDR_PIX_FMT_RGBA1010102,
} ultrahdr_pixel_format;

typedef enum {
  ULTRAHDR_NO_ERROR = 0,
  ULTRAHDR_UNKNOWN_ERROR = -1,

  ULTRAHDR_IO_ERROR_BASE = -10000,
  ERROR_ULTRAHDR_BAD_PTR = ULTRAHDR_IO_ERROR_BASE - 1,
  ERROR_ULTRAHDR_UNSUPPORTED_WIDTH_HEIGHT = ULTRAHDR_IO_ERROR_BASE - 2,
  ERROR_ULTRAHDR_INVALID_COLORGAMUT = ULTRAHDR_IO_ERROR_BASE - 3,
  ERROR_ULTRAHDR_INVALID_STRIDE = ULTRAHDR_IO_ERROR_BASE - 4,
  ERROR_ULTRAHDR_INVALID_TRANS_FUNC = ULTRAHDR_IO_ERROR_BASE - 5,
  ERROR_ULTRAHDR_RESOLUTION_MISMATCH = ULTRAHDR_IO_ERROR_BASE - 6,
  ERROR_ULTRAHDR_INVALID_QUALITY_FACTOR = ULTRAHDR_IO_ERROR_BASE - 7,
  ERROR_ULTRAHDR_INVALID_DISPLAY_BOOST = ULTRAHDR_IO_ERROR_BASE - 8,
  ERROR_ULTRAHDR_INVALID_OUTPUT_FORMAT = ULTRAHDR_IO_ERROR_BASE - 9,
  ERROR_ULTRAHDR_BAD_METADATA = ULTRAHDR_IO_ERROR_BASE - 10,
  ERROR_ULTRAHDR_INVALID_CROPPING_PARAMETERS = ULTRAHDR_IO_ERROR_BASE - 11,

  ULTRAHDR_RUNTIME_ERROR_BASE = -20000,
  ERROR_ULTRAHDR_ENCODE_ERROR = ULTRAHDR_RUNTIME_ERROR_BASE - 1,
  ERROR_ULTRAHDR_DECODE_ERROR = ULTRAHDR_RUNTIME_ERROR_BASE - 2,
  ERROR_ULTRAHDR_GAIN_MAP_IMAGE_NOT_FOUND = ULTRAHDR_RUNTIME_ERROR_BASE - 3,
  ERROR_ULTRAHDR_BUFFER_TOO_SMALL = ULTRAHDR_RUNTIME_ERROR_BASE - 4,
  ERROR_ULTRAHDR_METADATA_ERROR = ULTRAHDR_RUNTIME_ERROR_BASE - 5,
  ERROR_ULTRAHDR_NO_IMAGES_FOUND = ULTRAHDR_RUNTIME_ERROR_BASE - 6,
  ERROR_ULTRAHDR_MULTIPLE_EXIFS_RECEIVED = ULTRAHDR_RUNTIME_ERROR_BASE - 7,
  ERROR_ULTRAHDR_UNSUPPORTED_MAP_SCALE_FACTOR = ULTRAHDR_RUNTIME_ERROR_BASE - 8,

  ERROR_ULTRAHDR_UNSUPPORTED_FEATURE = -30000,
} status_t;

/*
 * Holds information for gain map related metadata.
 *
 * Not: all values stored in linear. This differs from the metadata encoding in XMP, where
 * maxContentBoost (aka gainMapMax), minContentBoost (aka gainMapMin), hdrCapacityMin, and
 * hdrCapacityMax are stored in log2 space.
 */
struct ultrahdr_metadata_struct {
  // Ultra HDR format version
  std::string version;
  // Max Content Boost for the map
  float maxContentBoost;
  // Min Content Boost for the map
  float minContentBoost;
  // Gamma of the map data
  float gamma;
  // Offset for SDR data in map calculations
  float offsetSdr;
  // Offset for HDR data in map calculations
  float offsetHdr;
  // HDR capacity to apply the map at all
  float hdrCapacityMin;
  // HDR capacity to apply the map completely
  float hdrCapacityMax;
};
typedef struct ultrahdr_metadata_struct* ultrahdr_metadata_ptr;

/*
 * Holds information for uncompressed image or gain map.
 */
struct ultrahdr_uncompressed_struct {
  // Pointer to the data location.
  void* data;
  // Width of the gain map or the luma plane of the image in pixels.
  size_t width;
  // Height of the gain map or the luma plane of the image in pixels.
  size_t height;
  // Color gamut.
  ultrahdr_color_gamut colorGamut;

  // Values below are optional
  // Pointer to chroma data, if it's NULL, chroma plane is considered to be immediately
  // after the luma plane.
  void* chroma_data = nullptr;
  // Stride of Y plane in number of pixels. 0 indicates the member is uninitialized. If
  // non-zero this value must be larger than or equal to luma width. If stride is
  // uninitialized then it is assumed to be equal to luma width.
  size_t luma_stride = 0;
  // Stride of UV plane in number of pixels.
  // 1. If this handle points to P010 image then this value must be larger than
  //    or equal to luma width.
  // 2. If this handle points to 420 image then this value must be larger than
  //    or equal to (luma width / 2).
  // NOTE: if chroma_data is nullptr, chroma_stride is irrelevant. Just as the way,
  // chroma_data is derived from luma ptr, chroma stride is derived from luma stride.
  size_t chroma_stride = 0;
  // Pixel format.
  ultrahdr_pixel_format pixelFormat = ULTRAHDR_PIX_FMT_UNSPECIFIED;
};
typedef struct ultrahdr_uncompressed_struct* uhdr_uncompressed_ptr;

/*
 * Holds information for compressed image or gain map.
 */
struct ultrahdr_compressed_struct {
  // Pointer to the data location.
  void* data;
  // Used data length in bytes.
  int length;
  // Maximum available data length in bytes.
  int maxLength;
  // Color gamut.
  ultrahdr_color_gamut colorGamut;
};
typedef struct ultrahdr_compressed_struct* uhdr_compressed_ptr;

/*
 * Holds information for EXIF metadata.
 */
struct ultrahdr_exif_struct {
  // Pointer to the data location.
  void* data;
  // Data length;
  size_t length;
};
typedef struct ultrahdr_exif_struct* uhdr_exif_ptr;

// The current gain map image version that we encode to
static const char* const kGainMapVersion = "1.0";

// Map is quarter res / sixteenth size
static const size_t kMapDimensionScaleFactor = 4;

class UltraHdr {
public:
  /*
   * This method is called in the encoding pipeline. It will take the uncompressed 8-bit and
   * 10-bit yuv images as input, and calculate the uncompressed gain map. The input images
   * must be the same resolution. The SDR input is assumed to use the sRGB transfer function.
   *
   * @param yuv420_image_ptr uncompressed SDR image in YUV_420 color format
   * @param p010_image_ptr uncompressed HDR image in P010 color format
   * @param hdr_tf transfer function of the HDR image
   * @param metadata everything but "version" is filled in this struct
   * @param dest location at which gain map image is stored (caller responsible for memory
                 of data).
   * @param sdr_is_601 if true, then use BT.601 decoding of YUV regardless of SDR image gamut
   * @return NO_ERROR if calculation succeeds, error code if error occurs.
   */
  static status_t generateGainMap(uhdr_uncompressed_ptr yuv420_image_ptr, uhdr_uncompressed_ptr p010_image_ptr,
                                  ultrahdr_transfer_function hdr_tf, ultrahdr_metadata_ptr metadata,
                                  uhdr_uncompressed_ptr dest, bool sdr_is_601 = false);

  /*
   * This method is called in the decoding pipeline. It will take the uncompressed (decoded)
   * 8-bit yuv image, the uncompressed (decoded) gain map, and extracted JPEG/R metadata as
   * input, and calculate the 10-bit recovered image. The recovered output image is the same
   * color gamut as the SDR image, with HLG transfer function, and is in RGBA1010102 data format.
   * The SDR image is assumed to use the sRGB transfer function. The SDR image is also assumed to
   * be a decoded JPEG for the purpose of YUV interpration.
   *
   * @param yuv420_image_ptr uncompressed SDR image in YUV_420 color format
   * @param gainmap_image_ptr pointer to uncompressed gain map image struct.
   * @param metadata JPEG/R metadata extracted from XMP.
   * @param output_format flag for setting output color format. if set to
   *                      {@code JPEGR_OUTPUT_SDR}, decoder will only decode the primary image
   *                      which is SDR. Default value is JPEGR_OUTPUT_HDR_LINEAR.
   * @param max_display_boost the maximum available boost supported by a display
   * @param dest reconstructed HDR image
   * @return NO_ERROR if calculation succeeds, error code if error occurs.
   */
  status_t applyGainMap(uhdr_uncompressed_ptr yuv420_image_ptr, uhdr_uncompressed_ptr gainmap_image_ptr,
                               ultrahdr_metadata_ptr metadata, ultrahdr_output_format output_format,
                               float max_display_boost, uhdr_uncompressed_ptr dest);

  /*
   * This method will tone map a HDR image to an SDR image.
   *
   * @param src pointer to uncompressed HDR image struct. HDR image is expected to be
   *            in p010 color format
   * @param dest pointer to store tonemapped SDR image
   */
  status_t toneMap(uhdr_uncompressed_ptr src, uhdr_uncompressed_ptr dest);
};


}  // namespace ultrahdr

#endif  // ULTRAHDR_ULTRAHDR_H
