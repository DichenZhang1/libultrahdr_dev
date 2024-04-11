/*
 * Copyright 2024 The Android Open Source Project
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

#ifndef ULTRAHDR_HEIFR_H
#define ULTRAHDR_HEIFR_H

#include <cfloat>

#include "ultrahdr/ultrahdr.h"
#include "ultrahdr/jpegdecoderhelper.h"
#include "ultrahdr/jpegencoderhelper.h"

#include "libheif/gain_map_metadata.h"
#include "libheif/heif.h"

namespace ultrahdr {
void read_one_plane(heif_image* img, heif_channel channel, int w, int h, void* data);
void fill_new_plane(heif_image* img, heif_channel channel, int w, int h, int s, void* data, int bit_depth = 8);
void read_image_as_p010(heif_image* img, int w, int h, void* data);
void convert_libheif_metadata_to_libultrahdr_metadata(const GainMapMetadata& from, ultrahdr_metadata_struct& to);

class MemoryWriter {
public:
  MemoryWriter() : data_(nullptr), size_(0), capacity_(0) {}

  ~MemoryWriter() {
    free(data_);
  }

  const uint8_t* data() const {
    return data_;
  }

  size_t size() const {
    return size_;
  }

  void write(const void* data, size_t size) {
    if (capacity_ - size_ < size) {
      size_t new_capacity = capacity_ + size;
      uint8_t* new_data = static_cast<uint8_t*>(malloc(new_capacity));
      if (data_) {
        memcpy(new_data, data_, size_);
        free(data_);
      }
      data_ = new_data;
      capacity_ = new_capacity;
    }
    memcpy(&data_[size_], data, size);
    size_ += size;
  }

public:
  uint8_t* data_;
  size_t size_;
  size_t capacity_;
};

class HeifR : public UltraHdr {
public:
  /*
   * Experimental only
   *
   * Encode API-0
   * Compress HEIFR image from 10-bit HDR YUV.
   *
   * Tonemap the HDR input to a SDR image, generate gain map from the HDR and SDR images,
   * compress SDR YUV to 8-bit HEIF and append the gain map to the end of the compressed
   * HEIF.
   * @param p010_image_ptr uncompressed HDR image in P010 color format
   * @param hdr_tf transfer function of the HDR image
   * @param dest destination of the compressed HEIFR image. Please note that {@code maxLength}
   *             represents the maximum available size of the destination buffer, and it must be
   *             set before calling this method. If the encoded HEIFR size exceeds
   *             {@code maxLength}, this method will return {@code ERROR_ULTRAHDR_BUFFER_TOO_SMALL}.
   * @param quality target quality of the HEIF encoding, must be in range of 0-100 where 100 is
   *                the highest quality
   * @param codec target output image codec (HEIC for HEVC codec, AVIF for AV1 codec)
   * @param exif pointer to the exif metadata.
   * @return NO_ERROR if encoding succeeds, error code if error occurs.
   */
  status_t encodeHeifWithGainMap(uhdr_uncompressed_ptr p010_image_ptr,
                                 ultrahdr_transfer_function hdr_tf,
                                 uhdr_compressed_ptr dest,
                                 int quality,
                                 ultrahdr_codec codec,
                                 uhdr_exif_ptr exif);

  /*
   * Encode API-1
   * Compress HEIFR image from 10-bit HDR YUV and 8-bit SDR YUV.
   *
   * Generate gain map from the HDR and SDR inputs, compress SDR YUV to 8-bit HEIF and append
   * the gain map to the end of the compressed HEIF. HDR and SDR inputs must be the same
   * resolution. SDR input is assumed to use the sRGB transfer function.
   * @param p010_image_ptr uncompressed HDR image in P010 color format
   * @param yuv420_image_ptr uncompressed SDR image in YUV_420 color format
   * @param hdr_tf transfer function of the HDR image
   * @param dest destination of the compressed HEIFR image. Please note that {@code maxLength}
   *             represents the maximum available size of the desitination buffer, and it must be
   *             set before calling this method. If the encoded HEIFR size exceeds
   *             {@code maxLength}, this method will return {@code ERROR_ULTRAHDR_BUFFER_TOO_SMALL}.
   * @param quality target quality of the HEIF encoding, must be in range of 0-100 where 100 is
   *                the highest quality
   * @param codec target output image codec (HEIC for HEVC codec, AVIF for AV1 codec)
   * @param exif pointer to the exif metadata.
   * @return NO_ERROR if encoding succeeds, error code if error occurs.
   */
  status_t encodeHeifWithGainMap(uhdr_uncompressed_ptr p010_image_ptr,
                                 uhdr_uncompressed_ptr yuv420_image_ptr,
                                 ultrahdr_transfer_function hdr_tf,
                                 uhdr_compressed_ptr dest,
                                 int quality,
                                 ultrahdr_codec codec,
                                 uhdr_exif_ptr exif);

  /*
   * Encode API-x
   * Compress HEIFR image from SDR YUV and raw gain map.
   *
   * This method is only used for transcoding case.
   *
   * @param yuv420_image_ptr uncompressed SDR image in YUV_420 color format
   * @param gainmap_image_ptr uncompressed gain map image in Y single channel color format
   * @param metadata gain map metadata to be written in the primary image
   * @param dest destination of the compressed HEIFR image. Please note that {@code maxLength}
   *             represents the maximum available size of the desitination buffer, and it must be
   *             set before calling this method. If the encoded HEIFR size exceeds
   *             {@code maxLength}, this method will return {@code ERROR_JPEGR_BUFFER_TOO_SMALL}.
   * @param quality target quality of the JPEG encoding, must be in range of 0-100 where 100 is
   *                the highest quality
   * @param exif pointer to the exif metadata.
   * @param codec target output image codec (HEIC for HEVC codec, AVIF for AV1 codec)
   * @return NO_ERROR if encoding succeeds, error code if error occurs.
   */
  status_t encodeHeifWithGainMap(uhdr_uncompressed_ptr yuv420_image_ptr,
                                 uhdr_uncompressed_ptr gainmap_image_ptr,
                                 ultrahdr_metadata_ptr metadata,
                                 uhdr_compressed_ptr dest, int quality,
                                 ultrahdr_codec codec,
                                 uhdr_exif_ptr exif);

  /*
   * Decode API
   * Decompress HEIFR image.
   *
   * This method assumes that the HEIFR image contains an ICC profile with primaries that match
   * those of a color gamut that this library is aware of; Bt.709, Display-P3, or Bt.2100. It also
   * assumes the base image uses the sRGB transfer function.
   *
   * This method only supports single gain map metadata values for fields that allow multi-channel
   * metadata values.
   * @param ultrahdr_image_ptr compressed HEIFR image.
   * @param dest destination of the uncompressed HEIFR image.
   * @param max_display_boost (optional) the maximum available boost supported by a display,
   *                          the value must be greater than or equal to 1.0.
   * @param exif destination of the decoded EXIF metadata. The default value is NULL where the
                 decoder will do nothing about it. If configured not NULL the decoder will write
                 EXIF data into this structure. The format is defined in {@code ultrahdr_exif_struct}
   * @param output_format flag for setting output color format. Its value configures the output
                          color format. The default value is {@code ULATRAHDR_OUTPUT_HDR_LINEAR}.
                          ----------------------------------------------------------------------
                          |        output_format       |    decoded color format to be written   |
                          ----------------------------------------------------------------------
                          |     ULTRAHDR_OUTPUT_SDR    |                RGBA_8888                |
                          ----------------------------------------------------------------------
                          | ULTRAHDR_OUTPUT_HDR_LINEAR |        (default)RGBA_F16 linear         |
                          ----------------------------------------------------------------------
                          |   ULTRAHDR_OUTPUT_HDR_PQ   |             RGBA_1010102 PQ             |
                          ---------------------------------------------------------------------
                          |  ULTRAHDR_OUTPUT_HDR_HLG   |            RGBA_1010102 HLG             |
                          ----------------------------------------------------------------------
   * @param gainmap_image_ptr destination of the decoded gain map. The default value is NULL
                              where the decoder will do nothing about it. If configured not NULL
                              the decoder will write the decoded gain_map data into this
                              structure. The format is defined in
                              {@code ultrahdr_uncompressed_struct}.
   * @param metadata destination of the decoded metadata. The default value is NULL where the
                     decoder will do nothing about it. If configured not NULL the decoder will
                     write metadata into this structure. the format of metadata is defined in
                     {@code ultrahdr_metadata_struct}.
   * @return NO_ERROR if decoding succeeds, error code if error occurs.
   */
  status_t decodeHeifWithGainMap(uhdr_compressed_ptr heifr_image_ptr,
                                 uhdr_uncompressed_ptr dest,
                                 float max_display_boost = FLT_MAX,
                                 uhdr_exif_ptr exif = nullptr,
                                 ultrahdr_output_format output_format = ULTRAHDR_OUTPUT_HDR_LINEAR,
                                 uhdr_uncompressed_ptr gainmap_image_ptr = nullptr,
                                 ultrahdr_metadata_ptr metadata = nullptr);
};
}  // namespace ultrahdr

#endif  // ULTRAHDR_HEIFR_H
