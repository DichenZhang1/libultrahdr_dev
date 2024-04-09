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

#ifdef _WIN32
#include <Windows.h>
#include <sysinfoapi.h>
#else
#include <unistd.h>
#endif

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string.h>
#include <thread>

#include "ultrahdr/ultrahdrcommon.h"
#include "ultrahdr/heifr.h"
#include "ultrahdr/gainmapmath.h"

#include "libheif/api_structs.h"
#include "libheif/pixelimage.h"

using namespace std;

namespace ultrahdr {
// HEIC / AVIF compress quality (0 ~ 100) for gain map
static const int kMapCompressQuality = 85;

static struct heif_error writer_write(struct heif_context* ctx, const void* data, size_t size, void* userdata) {
  MemoryWriter* writer = static_cast<MemoryWriter*>(userdata);
  writer->write(data, size);
  struct heif_error err{heif_error_Ok, heif_suberror_Unspecified, nullptr};
  return err;
}

void fill_new_plane(heif_image* img, heif_channel channel, int w, int h, int s, void* data, int bit_depth) {
  if (s == 0) {
    s = w;
  }

  struct heif_error err;
  err = heif_image_add_plane(img, channel, w, h, bit_depth);
  if (err.code != heif_error_Ok) {
      return;
  }

  int stride;

  uint8_t* p = heif_image_get_plane(img, channel, &stride);

  for (int y = 0; y < h; y++) {
    uint8_t* src = (uint8_t*)data + y * s;
    memcpy(p + y * stride, src, w);
  }
}

void read_one_plane(heif_image* img, heif_channel channel, int w, int h, void* data) {
  int stride;
  uint8_t* p = heif_image_get_plane(img, channel, &stride);

  if (channel == heif_channel_interleaved) { w = w * 4; }

  uint8_t* dest;
  for (int y = 0; y < h; y++) {
    uint8_t* dest = (uint8_t*)data + y * w;
    memcpy(dest, p + y * stride, w);
  }
}

void read_image_as_p010(heif_image* img, int w, int h, void* data) {
  int y_stride;
  uint16_t* pY = (uint16_t*)heif_image_get_plane(img, heif_channel_Y, &y_stride);
  uint16_t* dest = (uint16_t*)data;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      *(dest + y * w + x) = *(pY + y * y_stride / 2 + x) << 6;
    }
  }

  int cb_stride;
  int cr_stride;
  uint16_t* pCb = (uint16_t*)heif_image_get_plane(img, heif_channel_Cb, &cb_stride);
  uint16_t* pCr = (uint16_t*)heif_image_get_plane(img, heif_channel_Cr, &cr_stride);
  dest =(uint16_t*)data + h * y_stride / 2;
  for (int y = 0; y < h / 2; y++) {
    for (int x = 0; x < w / 2; x++) {
      *(dest + y * w + x * 2) = *(pCb + y * cb_stride / 2 + x) << 6;
      *(dest + y * w + x * 2 + 1) = *(pCr + y * cr_stride / 2 + x) << 6;
    }
  }
}

void convert_libheif_metadata_to_libultrahdr_metadata(const GainMapMetadata& from, ultrahdr_metadata_struct& to) {
  to.version = kGainMapVersion;
  to.maxContentBoost = kHlgMaxNits / kSdrWhiteNits;
  to.minContentBoost = 1.0f;
  to.gamma = from.gainMapGammaN[0] / from.gainMapGammaD[0];
  to.offsetSdr = from.baseOffsetN[0] / from.baseOffsetD[0];
  to.offsetHdr = from.alternateOffsetN[0] / from.alternateOffsetD[0];
  to.hdrCapacityMin = to.minContentBoost;
  to.hdrCapacityMax = to.maxContentBoost;
}

void convert_libultrahdr_metadata_to_libheif_metadata(const ultrahdr_metadata_struct& from, GainMapMetadata& to) {
  to.backwardDirection = false;
  to.useBaseColorSpace = true;

  to.gainMapMinN[0]      = to.gainMapMinN[1]      = to.gainMapMinN[2]      = 1;
  to.gainMapMinD[0]      = to.gainMapMinD[1]      = to.gainMapMinD[2]      = 1;
  to.gainMapMaxN[0]      = to.gainMapMaxN[1]      = to.gainMapMaxN[2]      = kHlgMaxNits;
  to.gainMapMaxD[0]      = to.gainMapMaxD[1]      = to.gainMapMaxD[2]      = kSdrWhiteNits;
  to.gainMapGammaN[0]    = to.gainMapGammaN[1]    = to.gainMapGammaN[2]    = from.gamma * 1000000;
  to.gainMapGammaD[0]    = to.gainMapGammaD[1]    = to.gainMapGammaD[2]    = 1000000;
  to.baseOffsetN[0]      = to.baseOffsetN[1]      = to.baseOffsetN[2]      = from.offsetSdr * 1000000;
  to.baseOffsetD[0]      = to.baseOffsetD[1]      = to.baseOffsetD[2]      = 1000000;
  to.alternateOffsetN[0] = to.alternateOffsetN[1] = to.alternateOffsetN[2] = from.offsetHdr * 1000000;
  to.alternateOffsetD[0] = to.alternateOffsetD[1] = to.alternateOffsetD[2] = 1000000;

  to.baseHdrHeadroomN = 0;
  to.baseHdrHeadroomD = 0;
  to.alternateHdrHeadroomN = 0;
  to.alternateHdrHeadroomD = 0;
}

/* Encode API-0 */
status_t HeifR::encodeHeifWithGainMap(uhdr_uncompressed_ptr p010_image_ptr,
                                      ultrahdr_transfer_function hdr_tf,
                                      uhdr_compressed_ptr dest,
                                      int quality,
                                      ultrahdr_codec codec,
                                      uhdr_exif_ptr exif) {

  // clean up input structure for later usage
  ultrahdr_uncompressed_struct p010_image = *p010_image_ptr;
  if (p010_image.luma_stride == 0) p010_image.luma_stride = p010_image.width;
  if (!p010_image.chroma_data) {
    uint16_t* data = reinterpret_cast<uint16_t*>(p010_image.data);
    p010_image.chroma_data = data + p010_image.luma_stride * p010_image.height;
    p010_image.chroma_stride = p010_image.luma_stride;
  }

  const size_t yu420_luma_stride = p010_image.luma_stride;

  unique_ptr<uint8_t[]> yuv420_image_data =
      make_unique<uint8_t[]>(yu420_luma_stride * p010_image.height * 3 / 2);
  ultrahdr_uncompressed_struct yuv420_image;
  yuv420_image.data = yuv420_image_data.get();
  yuv420_image.width = p010_image.width;
  yuv420_image.height = p010_image.height;
  yuv420_image.colorGamut = p010_image.colorGamut;
  yuv420_image.chroma_data = nullptr;
  yuv420_image.luma_stride = yu420_luma_stride;
  yuv420_image.chroma_stride = yu420_luma_stride >> 1;
  uint8_t* data = reinterpret_cast<uint8_t*>(yuv420_image.data);
  yuv420_image.chroma_data = data + yuv420_image.luma_stride * yuv420_image.height;

  // tone map
  ULTRAHDR_CHECK(toneMap(&p010_image, &yuv420_image));

  return encodeHeifWithGainMap(&p010_image, &yuv420_image, hdr_tf, dest, quality, codec, exif);
}

/* Encode API-1 */
status_t HeifR::encodeHeifWithGainMap(uhdr_uncompressed_ptr p010_image_ptr,
                                      uhdr_uncompressed_ptr yuv420_image_ptr,
                                      ultrahdr_transfer_function hdr_tf,
                                      uhdr_compressed_ptr dest,
                                      int quality,
                                      ultrahdr_codec codec,
                                      uhdr_exif_ptr exif) {
  // clean up input structure for later usage
  ultrahdr_uncompressed_struct p010_image = *p010_image_ptr;
  if (p010_image.luma_stride == 0) p010_image.luma_stride = p010_image.width;
  if (!p010_image.chroma_data) {
    uint16_t* data = reinterpret_cast<uint16_t*>(p010_image.data);
    p010_image.chroma_data = data + p010_image.luma_stride * p010_image.height;
    p010_image.chroma_stride = p010_image.luma_stride;
  }
  ultrahdr_uncompressed_struct yuv420_image = *yuv420_image_ptr;
  if (yuv420_image.luma_stride == 0) yuv420_image.luma_stride = yuv420_image.width;
  if (!yuv420_image.chroma_data) {
    uint8_t* data = reinterpret_cast<uint8_t*>(yuv420_image.data);
    yuv420_image.chroma_data = data + yuv420_image.luma_stride * yuv420_image.height;
    yuv420_image.chroma_stride = yuv420_image.luma_stride >> 1;
  }

  // gain map
  ultrahdr_metadata_struct metadata;
  metadata.version = "1";
  ultrahdr_uncompressed_struct gainmap_image;
  generateGainMap(&yuv420_image, &p010_image, hdr_tf, &metadata, &gainmap_image);
  std::unique_ptr<uint8_t[]> map_data;
  map_data.reset(reinterpret_cast<uint8_t*>(gainmap_image.data));

  return encodeHeifWithGainMap(&yuv420_image, &gainmap_image, &metadata, dest, quality, codec, exif);
}

/* Encode API-x */
status_t HeifR::encodeHeifWithGainMap(uhdr_uncompressed_ptr yuv420_image_ptr,
                                      uhdr_uncompressed_ptr gainmap_image_ptr,
                                      ultrahdr_metadata_ptr metadata,
                                      uhdr_compressed_ptr dest, int quality,
                                      ultrahdr_codec codec,
                                      uhdr_exif_ptr exif) {
  int input_width = yuv420_image_ptr->width;
  int input_height = yuv420_image_ptr->height;

  // clean up input structure for later usage
  ultrahdr_uncompressed_struct yuv420_image = *yuv420_image_ptr;
  if (yuv420_image.luma_stride == 0) yuv420_image.luma_stride = yuv420_image.width;
  if (!yuv420_image.chroma_data) {
    uint8_t* data = reinterpret_cast<uint8_t*>(yuv420_image.data);
    yuv420_image.chroma_data = data + yuv420_image.luma_stride * yuv420_image.height;
    yuv420_image.chroma_stride = yuv420_image.luma_stride >> 1;
  }

  heif_context* ctx = heif_context_alloc();
  MemoryWriter writer;
  struct heif_writer w;
  w.writer_api_version = 1;
  w.write = writer_write;

  // get the default encoder
  heif_encoder* encoder;
  if (codec == ULTRAHDR_CODEC_HEIC_R ||
          codec == ULTRAHDR_CODEC_HEIC ||
          codec == ULTRAHDR_CODEC_HEIC_10_BIT ) {
    heif_context_get_encoder_for_format(ctx, heif_compression_HEVC, &encoder);
  } else if (codec == ULTRAHDR_CODEC_AVIF_R ||
          codec == ULTRAHDR_CODEC_AVIF ||
          codec == ULTRAHDR_CODEC_AVIF_10_BIT ) {
    heif_context_get_encoder_for_format(ctx, heif_compression_AV1, &encoder);
  } else {
    return ERROR_ULTRAHDR_UNSUPPORTED_FEATURE;
  }

  // set the encoder parameters
  heif_encoder_set_lossy_quality(encoder, quality);

  // encode the primary image
  heif_image* image;
  struct heif_image_handle* handle;
  heif_image_create(input_width, input_height, heif_colorspace_YCbCr, heif_chroma_420, &image);
  fill_new_plane(image, heif_channel_Y, input_width, input_height, yuv420_image_ptr->luma_stride, yuv420_image_ptr->data);
  fill_new_plane(image, heif_channel_Cb, (input_width + 1) / 2, (input_height + 1) / 2, yuv420_image_ptr->chroma_stride,
      (uint8_t*)yuv420_image_ptr->data + input_width * input_height);
  fill_new_plane(image, heif_channel_Cr, (input_width + 1) / 2, (input_height + 1) / 2, yuv420_image_ptr->chroma_stride,
      (uint8_t*)yuv420_image_ptr->data + input_width * input_height * 5 / 4);
  heif_context_encode_image(ctx, image, encoder, nullptr, &handle);

  // add exif
  if (exif != nullptr) {
    heif_context_add_exif_metadata(ctx, handle, exif->data, exif->length);
  }

  if (gainmap_image_ptr == nullptr && metadata == nullptr) {
    // only encode heif
    heif_encoder_release(encoder);
    heif_context_write(ctx, &w, &writer);
    memcpy(dest->data, writer.data(), writer.size());
    dest->length = writer.size();
    return ULTRAHDR_NO_ERROR;
  }

  // gain map metadata
  GainMapMetadata gmm;
  convert_libultrahdr_metadata_to_libheif_metadata(*metadata, gmm);

  // encode the gain map image
  heif_image* gain_map_image;
  struct heif_image_handle* gain_map_image_handle;
  heif_image_create(gainmap_image_ptr->width, gainmap_image_ptr->height, heif_colorspace_monochrome, heif_chroma_monochrome, &gain_map_image);
  fill_new_plane(gain_map_image, heif_channel_Y, gainmap_image_ptr->width, gainmap_image_ptr->height, gainmap_image_ptr->width, gainmap_image_ptr->data);
  heif_context_encode_gain_map_image(ctx, gain_map_image, handle, encoder, nullptr, &gmm, &gain_map_image_handle);

  heif_encoder_release(encoder);

  heif_context_write(ctx, &w, &writer);
  memcpy(dest->data, writer.data(), writer.size());
  dest->length = writer.size();

  heif_context_free(ctx);
  return ULTRAHDR_NO_ERROR;
}

/* Decode API */
status_t HeifR::decodeHeifWithGainMap(uhdr_compressed_ptr heifr_image_ptr,
                                      uhdr_uncompressed_ptr dest,
                                      float max_display_boost,
                                      uhdr_exif_ptr exif,
                                      ultrahdr_output_format output_format,
                                      uhdr_uncompressed_ptr gainmap_image_ptr,
                                      ultrahdr_metadata_ptr out_metadata) {
  heif_context* ctx = heif_context_alloc();
  heif_context_read_from_memory_without_copy(ctx, heifr_image_ptr->data, heifr_image_ptr->length, nullptr);

  if (output_format == ULTRAHDR_OUTPUT_SDR) {
    heif_image_handle* handle;
    heif_image* image;
    heif_context_get_primary_image_handle(ctx, &handle);
    heif_decode_image(handle, &image, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, nullptr);
    int width = image->image->get_width();
    int height = image->image->get_height();
//    dest->data = malloc(width * height * 4);
    dest->width = width;
    dest->height = height;
    read_one_plane(image, heif_channel_interleaved, width, height, dest->data);

    // exif
    if (exif != nullptr) {
      heif_item_id exif_id;
      int n = heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", &exif_id, 1);
      if (n == 1) {
        exif->length = heif_image_handle_get_metadata_size(handle, exif_id);
        struct heif_error error = heif_image_handle_get_metadata(handle, exif_id, (uint8_t*) exif->data);
      }
    }

    return ULTRAHDR_NO_ERROR;
  }

  // primary image
  heif_image_handle* handle;
  heif_image* image;
  heif_context_get_primary_image_handle(ctx, &handle);
  heif_decode_image(handle, &image, heif_colorspace_YCbCr, heif_chroma_420, nullptr);
  ultrahdr_uncompressed_struct yuv420;
  int width = image->image->get_width();
  int height = image->image->get_height();
  std::unique_ptr<uint8_t[]> yuv420_data = make_unique<uint8_t[]>(width * height * 3 / 2);
  yuv420.data = yuv420_data.get();
  yuv420.chroma_data = (uint8_t*)yuv420.data + width * height;
  yuv420.width = width;
  yuv420.height = height;
  read_one_plane(image, heif_channel_Y, width, height, yuv420.data);
  read_one_plane(image, heif_channel_Cb, (width + 1) / 2, (height + 1) / 2, (uint8_t*)yuv420.data + width * height);
  read_one_plane(image, heif_channel_Cr, (width + 1) / 2, (height + 1) / 2, (uint8_t*)yuv420.data + width * height * 5 / 4);

  // exif
  if (exif != nullptr) {
    heif_item_id exif_id;
    int n = heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", &exif_id, 1);
    if (n == 1) {
      exif->length = heif_image_handle_get_metadata_size(handle, exif_id);
      struct heif_error error = heif_image_handle_get_metadata(handle, exif_id, (uint8_t*) exif->data);
    }
  }

  // gain map image
  heif_image_handle* gain_map_image_handle;
  heif_image* gain_map_image;
  if (struct heif_error err = heif_context_get_gain_map_image_handle(ctx, &gain_map_image_handle);
          err.code != heif_error_Ok) {
    return ERROR_ULTRAHDR_GAIN_MAP_IMAGE_NOT_FOUND;
  }
  heif_decode_image(gain_map_image_handle, &gain_map_image, heif_colorspace_undefined, heif_chroma_undefined, nullptr);
  ultrahdr_uncompressed_struct gainmap;
  int gm_width = gain_map_image->image->get_width();
  int gm_height = gain_map_image->image->get_height();

  std::unique_ptr<uint8_t[]> gainmap_data = make_unique<uint8_t[]>(gm_width * gm_height);
  gainmap.data = gainmap_data.get();
  gainmap.width = gm_width;
  gainmap.height = gm_height;
  read_one_plane(gain_map_image, heif_channel_Y, gm_width, gm_height, gainmap.data);
  if (gainmap_image_ptr != nullptr) {
    gainmap_image_ptr->width = gainmap.width;
    gainmap_image_ptr->height = gainmap.height;
    int size = gainmap_image_ptr->width * gainmap_image_ptr->height;
    gainmap_image_ptr->data = malloc(size);
    memcpy(gainmap_image_ptr->data, gainmap.data, size);
  }

  // gain map metadata
  GainMapMetadata gmm;
  heif_image_get_gain_map_metadata(ctx, &gmm);
  ultrahdr_metadata_struct metadata;
  convert_libheif_metadata_to_libultrahdr_metadata(gmm, metadata);
  if (out_metadata != nullptr) {
    out_metadata->version = metadata.version;
    out_metadata->minContentBoost = metadata.minContentBoost;
    out_metadata->maxContentBoost = metadata.maxContentBoost;
    out_metadata->gamma = metadata.gamma;
    out_metadata->offsetSdr = metadata.offsetSdr;
    out_metadata->offsetHdr = metadata.offsetHdr;
    out_metadata->hdrCapacityMin = metadata.hdrCapacityMin;
    out_metadata->hdrCapacityMax = metadata.hdrCapacityMax;
  }

  // decoded result
  if (dest != nullptr) {
    applyGainMap(&yuv420, &gainmap, &metadata, output_format, max_display_boost, dest);
  }
  return ULTRAHDR_NO_ERROR;
}

}  // namespace ultrahdr
