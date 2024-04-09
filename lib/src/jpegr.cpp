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

#include "ultrahdr/ultrahdrcommon.h"
#include "ultrahdr/jpegr.h"
#include "ultrahdr/icc.h"
#include "ultrahdr/multipictureformat.h"

#include "image_io/base/data_segment_data_source.h"
#include "image_io/jpeg/jpeg_info.h"
#include "image_io/jpeg/jpeg_info_builder.h"
#include "image_io/jpeg/jpeg_marker.h"
#include "image_io/jpeg/jpeg_scanner.h"

using namespace std;
using namespace photos_editing_formats::image_io;

namespace ultrahdr {

#define USE_SRGB_INVOETF_LUT 1
#define USE_HLG_OETF_LUT 1
#define USE_PQ_OETF_LUT 1
#define USE_HLG_INVOETF_LUT 1
#define USE_PQ_INVOETF_LUT 1
#define USE_APPLY_GAIN_LUT 1

// JPEG compress quality (0 ~ 100) for gain map
static const int kMapCompressQuality = 85;

/*
 * MessageWriter implementation for ALOG functions.
 */
class AlogMessageWriter : public MessageWriter {
 public:
  void WriteMessage(const Message& message) override {
    std::string log = GetFormattedMessage(message);
    ALOGD("%s", log.c_str());
  }
};

/*
 * Helper function copies the JPEG image from without EXIF.
 *
 * @param pDest destination of the data to be written.
 * @param pSource source of data being written.
 * @param exif_pos position of the EXIF package, which is aligned with jpegdecoder.getEXIFPos().
 *                 (4 bytes offset to FF sign, the byte after FF E1 XX XX <this byte>).
 * @param exif_size exif size without the initial 4 bytes, aligned with jpegdecoder.getEXIFSize().
 */
static void copyJpegWithoutExif(uhdr_compressed_ptr pDest, uhdr_compressed_ptr pSource, size_t exif_pos,
                                size_t exif_size) {
  const size_t exif_offset = 4;  // exif_pos has 4 bytes offset to the FF sign
  pDest->length = pSource->length - exif_size - exif_offset;
  pDest->data = new uint8_t[pDest->length];
  pDest->maxLength = pDest->length;
  pDest->colorGamut = pSource->colorGamut;
  memcpy(pDest->data, pSource->data, exif_pos - exif_offset);
  memcpy((uint8_t*)pDest->data + exif_pos - exif_offset,
         (uint8_t*)pSource->data + exif_pos + exif_size, pSource->length - exif_pos - exif_size);
}

status_t JpegR::areInputArgumentsValid(uhdr_uncompressed_ptr p010_image_ptr,
                                       uhdr_uncompressed_ptr yuv420_image_ptr,
                                       ultrahdr_transfer_function hdr_tf,
                                       uhdr_compressed_ptr dest_ptr) {
  if (p010_image_ptr == nullptr || p010_image_ptr->data == nullptr) {
    ALOGE("Received nullptr for input p010 image");
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (p010_image_ptr->width % 2 != 0 || p010_image_ptr->height % 2 != 0) {
    ALOGE("Image dimensions cannot be odd, image dimensions %zux%zu", p010_image_ptr->width,
          p010_image_ptr->height);
    return ERROR_ULTRAHDR_UNSUPPORTED_WIDTH_HEIGHT;
  }
  if (p010_image_ptr->width < kMinWidth || p010_image_ptr->height < kMinHeight) {
    ALOGE("Image dimensions cannot be less than %dx%d, image dimensions %zux%zu", kMinWidth,
          kMinHeight, p010_image_ptr->width, p010_image_ptr->height);
    return ERROR_ULTRAHDR_UNSUPPORTED_WIDTH_HEIGHT;
  }
  if (p010_image_ptr->width > kMaxWidth || p010_image_ptr->height > kMaxHeight) {
    ALOGE("Image dimensions cannot be larger than %dx%d, image dimensions %zux%zu", kMaxWidth,
          kMaxHeight, p010_image_ptr->width, p010_image_ptr->height);
    return ERROR_ULTRAHDR_UNSUPPORTED_WIDTH_HEIGHT;
  }
  if (p010_image_ptr->colorGamut <= ULTRAHDR_COLORGAMUT_UNSPECIFIED ||
      p010_image_ptr->colorGamut > ULTRAHDR_COLORGAMUT_MAX) {
    ALOGE("Unrecognized p010 color gamut %d", p010_image_ptr->colorGamut);
    return ERROR_ULTRAHDR_INVALID_COLORGAMUT;
  }
  if (p010_image_ptr->luma_stride != 0 && p010_image_ptr->luma_stride < p010_image_ptr->width) {
    ALOGE("Luma stride must not be smaller than width, stride=%zu, width=%zu",
          p010_image_ptr->luma_stride, p010_image_ptr->width);
    return ERROR_ULTRAHDR_INVALID_STRIDE;
  }
  if (p010_image_ptr->chroma_data != nullptr &&
      p010_image_ptr->chroma_stride < p010_image_ptr->width) {
    ALOGE("Chroma stride must not be smaller than width, stride=%zu, width=%zu",
          p010_image_ptr->chroma_stride, p010_image_ptr->width);
    return ERROR_ULTRAHDR_INVALID_STRIDE;
  }
  if (dest_ptr == nullptr || dest_ptr->data == nullptr) {
    ALOGE("Received nullptr for destination");
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (hdr_tf <= ULTRAHDR_TF_UNSPECIFIED || hdr_tf > ULTRAHDR_TF_MAX || hdr_tf == ULTRAHDR_TF_SRGB) {
    ALOGE("Invalid hdr transfer function %d", hdr_tf);
    return ERROR_ULTRAHDR_INVALID_TRANS_FUNC;
  }
  if (yuv420_image_ptr == nullptr) {
    return ULTRAHDR_NO_ERROR;
  }
  if (yuv420_image_ptr->data == nullptr) {
    ALOGE("Received nullptr for uncompressed 420 image");
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (yuv420_image_ptr->luma_stride != 0 &&
      yuv420_image_ptr->luma_stride < yuv420_image_ptr->width) {
    ALOGE("Luma stride must not be smaller than width, stride=%zu, width=%zu",
          yuv420_image_ptr->luma_stride, yuv420_image_ptr->width);
    return ERROR_ULTRAHDR_INVALID_STRIDE;
  }
  if (yuv420_image_ptr->chroma_data != nullptr &&
      yuv420_image_ptr->chroma_stride < yuv420_image_ptr->width / 2) {
    ALOGE("Chroma stride must not be smaller than (width / 2), stride=%zu, width=%zu",
          yuv420_image_ptr->chroma_stride, yuv420_image_ptr->width);
    return ERROR_ULTRAHDR_INVALID_STRIDE;
  }
  if (p010_image_ptr->width != yuv420_image_ptr->width ||
      p010_image_ptr->height != yuv420_image_ptr->height) {
    ALOGE("Image resolutions mismatch: P010: %zux%zu, YUV420: %zux%zu", p010_image_ptr->width,
          p010_image_ptr->height, yuv420_image_ptr->width, yuv420_image_ptr->height);
    return ERROR_ULTRAHDR_RESOLUTION_MISMATCH;
  }
  if (yuv420_image_ptr->colorGamut <= ULTRAHDR_COLORGAMUT_UNSPECIFIED ||
      yuv420_image_ptr->colorGamut > ULTRAHDR_COLORGAMUT_MAX) {
    ALOGE("Unrecognized 420 color gamut %d", yuv420_image_ptr->colorGamut);
    return ERROR_ULTRAHDR_INVALID_COLORGAMUT;
  }
  return ULTRAHDR_NO_ERROR;
}

status_t JpegR::areInputArgumentsValid(uhdr_uncompressed_ptr p010_image_ptr,
                                       uhdr_uncompressed_ptr yuv420_image_ptr,
                                       ultrahdr_transfer_function hdr_tf,
                                       uhdr_compressed_ptr dest_ptr, int quality) {
  if (quality < 0 || quality > 100) {
    ALOGE("quality factor is out side range [0-100], quality factor : %d", quality);
    return ERROR_ULTRAHDR_INVALID_QUALITY_FACTOR;
  }
  return areInputArgumentsValid(p010_image_ptr, yuv420_image_ptr, hdr_tf, dest_ptr);
}

/* Encode API-0 */
status_t JpegR::encodeJPEGR(uhdr_uncompressed_ptr p010_image_ptr, ultrahdr_transfer_function hdr_tf,
                            uhdr_compressed_ptr dest, int quality, uhdr_exif_ptr exif) {
  // validate input arguments
  ULTRAHDR_CHECK(areInputArgumentsValid(p010_image_ptr, nullptr, hdr_tf, dest, quality));
  if (exif != nullptr && exif->data == nullptr) {
    ALOGE("received nullptr for exif metadata");
    return ERROR_ULTRAHDR_BAD_PTR;
  }

  // clean up input structure for later usage
  ultrahdr_uncompressed_struct p010_image = *p010_image_ptr;
  if (p010_image.luma_stride == 0) p010_image.luma_stride = p010_image.width;
  if (!p010_image.chroma_data) {
    uint16_t* data = reinterpret_cast<uint16_t*>(p010_image.data);
    p010_image.chroma_data = data + p010_image.luma_stride * p010_image.height;
    p010_image.chroma_stride = p010_image.luma_stride;
  }

  const size_t yu420_luma_stride = ALIGNM(p010_image.width, JpegEncoderHelper::kCompressBatchSize);
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

  // gain map
  ultrahdr_metadata_struct metadata;
  metadata.version = kGainMapVersion;
  ultrahdr_uncompressed_struct gainmap_image;
  ULTRAHDR_CHECK(generateGainMap(&yuv420_image, &p010_image, hdr_tf, &metadata, &gainmap_image));
  std::unique_ptr<uint8_t[]> map_data;
  map_data.reset(reinterpret_cast<uint8_t*>(gainmap_image.data));

  // compress gain map
  JpegEncoderHelper jpeg_enc_obj_gm;
  ULTRAHDR_CHECK(compressGainMap(&gainmap_image, &jpeg_enc_obj_gm));
  ultrahdr_compressed_struct compressed_map;
  compressed_map.data = jpeg_enc_obj_gm.getCompressedImagePtr();
  compressed_map.length = static_cast<int>(jpeg_enc_obj_gm.getCompressedImageSize());
  compressed_map.maxLength = static_cast<int>(jpeg_enc_obj_gm.getCompressedImageSize());
  compressed_map.colorGamut = ULTRAHDR_COLORGAMUT_UNSPECIFIED;

  std::shared_ptr<DataStruct> icc =
      IccHelper::writeIccProfile(ULTRAHDR_TF_SRGB, yuv420_image.colorGamut);

  // convert to Bt601 YUV encoding for JPEG encode
  if (yuv420_image.colorGamut != ULTRAHDR_COLORGAMUT_P3) {
    ULTRAHDR_CHECK(convertYuv(&yuv420_image, yuv420_image.colorGamut, ULTRAHDR_COLORGAMUT_P3));
  }

  // compress 420 image
  JpegEncoderHelper jpeg_enc_obj_yuv420;
  if (!jpeg_enc_obj_yuv420.compressImage(reinterpret_cast<uint8_t*>(yuv420_image.data),
                                         reinterpret_cast<uint8_t*>(yuv420_image.chroma_data),
                                         yuv420_image.width, yuv420_image.height,
                                         yuv420_image.luma_stride, yuv420_image.chroma_stride,
                                         quality, icc->getData(), icc->getLength())) {
    return ERROR_ULTRAHDR_ENCODE_ERROR;
  }
  ultrahdr_compressed_struct jpeg;
  jpeg.data = jpeg_enc_obj_yuv420.getCompressedImagePtr();
  jpeg.length = static_cast<int>(jpeg_enc_obj_yuv420.getCompressedImageSize());
  jpeg.maxLength = static_cast<int>(jpeg_enc_obj_yuv420.getCompressedImageSize());
  jpeg.colorGamut = yuv420_image.colorGamut;

  // append gain map, no ICC since JPEG encode already did it
  ULTRAHDR_CHECK(appendGainMap(&jpeg, &compressed_map, exif, /* icc */ nullptr, /* icc size */ 0,
                            &metadata, dest));

  return ULTRAHDR_NO_ERROR;
}

/* Encode API-1 */
status_t JpegR::encodeJPEGR(uhdr_uncompressed_ptr p010_image_ptr,
                            uhdr_uncompressed_ptr yuv420_image_ptr, ultrahdr_transfer_function hdr_tf,
                            uhdr_compressed_ptr dest, int quality, uhdr_exif_ptr exif) {
  // validate input arguments
  if (yuv420_image_ptr == nullptr) {
    ALOGE("received nullptr for uncompressed 420 image");
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (exif != nullptr && exif->data == nullptr) {
    ALOGE("received nullptr for exif metadata");
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  ULTRAHDR_CHECK(areInputArgumentsValid(p010_image_ptr, yuv420_image_ptr, hdr_tf, dest, quality))

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
  metadata.version = kGainMapVersion;
  ultrahdr_uncompressed_struct gainmap_image;
  ULTRAHDR_CHECK(generateGainMap(&yuv420_image, &p010_image, hdr_tf, &metadata, &gainmap_image));
  std::unique_ptr<uint8_t[]> map_data;
  map_data.reset(reinterpret_cast<uint8_t*>(gainmap_image.data));

  // compress gain map
  JpegEncoderHelper jpeg_enc_obj_gm;
  ULTRAHDR_CHECK(compressGainMap(&gainmap_image, &jpeg_enc_obj_gm));
  ultrahdr_compressed_struct compressed_map;
  compressed_map.data = jpeg_enc_obj_gm.getCompressedImagePtr();
  compressed_map.length = static_cast<int>(jpeg_enc_obj_gm.getCompressedImageSize());
  compressed_map.maxLength = static_cast<int>(jpeg_enc_obj_gm.getCompressedImageSize());
  compressed_map.colorGamut = ULTRAHDR_COLORGAMUT_UNSPECIFIED;

  std::shared_ptr<DataStruct> icc =
      IccHelper::writeIccProfile(ULTRAHDR_TF_SRGB, yuv420_image.colorGamut);

  ultrahdr_uncompressed_struct yuv420_bt601_image = yuv420_image;
  unique_ptr<uint8_t[]> yuv_420_bt601_data;
  // Convert to bt601 YUV encoding for JPEG encode
  if (yuv420_image.colorGamut != ULTRAHDR_COLORGAMUT_P3) {
    const size_t yuv_420_bt601_luma_stride =
        ALIGNM(yuv420_image.width, JpegEncoderHelper::kCompressBatchSize);
    yuv_420_bt601_data =
        make_unique<uint8_t[]>(yuv_420_bt601_luma_stride * yuv420_image.height * 3 / 2);
    yuv420_bt601_image.data = yuv_420_bt601_data.get();
    yuv420_bt601_image.colorGamut = yuv420_image.colorGamut;
    yuv420_bt601_image.luma_stride = yuv_420_bt601_luma_stride;
    uint8_t* data = reinterpret_cast<uint8_t*>(yuv420_bt601_image.data);
    yuv420_bt601_image.chroma_data = data + yuv_420_bt601_luma_stride * yuv420_image.height;
    yuv420_bt601_image.chroma_stride = yuv_420_bt601_luma_stride >> 1;

    {
      // copy luma
      uint8_t* y_dst = reinterpret_cast<uint8_t*>(yuv420_bt601_image.data);
      uint8_t* y_src = reinterpret_cast<uint8_t*>(yuv420_image.data);
      if (yuv420_bt601_image.luma_stride == yuv420_image.luma_stride) {
        memcpy(y_dst, y_src, yuv420_bt601_image.luma_stride * yuv420_image.height);
      } else {
        for (size_t i = 0; i < yuv420_image.height; i++) {
          memcpy(y_dst, y_src, yuv420_image.width);
          if (yuv420_image.width != yuv420_bt601_image.luma_stride) {
            memset(y_dst + yuv420_image.width, 0,
                   yuv420_bt601_image.luma_stride - yuv420_image.width);
          }
          y_dst += yuv420_bt601_image.luma_stride;
          y_src += yuv420_image.luma_stride;
        }
      }
    }

    if (yuv420_bt601_image.chroma_stride == yuv420_image.chroma_stride) {
      // copy luma
      uint8_t* ch_dst = reinterpret_cast<uint8_t*>(yuv420_bt601_image.chroma_data);
      uint8_t* ch_src = reinterpret_cast<uint8_t*>(yuv420_image.chroma_data);
      memcpy(ch_dst, ch_src, yuv420_bt601_image.chroma_stride * yuv420_image.height);
    } else {
      // copy cb & cr
      uint8_t* cb_dst = reinterpret_cast<uint8_t*>(yuv420_bt601_image.chroma_data);
      uint8_t* cb_src = reinterpret_cast<uint8_t*>(yuv420_image.chroma_data);
      uint8_t* cr_dst = cb_dst + (yuv420_bt601_image.chroma_stride * yuv420_bt601_image.height / 2);
      uint8_t* cr_src = cb_src + (yuv420_image.chroma_stride * yuv420_image.height / 2);
      for (size_t i = 0; i < yuv420_image.height / 2; i++) {
        memcpy(cb_dst, cb_src, yuv420_image.width / 2);
        memcpy(cr_dst, cr_src, yuv420_image.width / 2);
        if (yuv420_bt601_image.width / 2 != yuv420_bt601_image.chroma_stride) {
          memset(cb_dst + yuv420_image.width / 2, 0,
                 yuv420_bt601_image.chroma_stride - yuv420_image.width / 2);
          memset(cr_dst + yuv420_image.width / 2, 0,
                 yuv420_bt601_image.chroma_stride - yuv420_image.width / 2);
        }
        cb_dst += yuv420_bt601_image.chroma_stride;
        cb_src += yuv420_image.chroma_stride;
        cr_dst += yuv420_bt601_image.chroma_stride;
        cr_src += yuv420_image.chroma_stride;
      }
    }
    ULTRAHDR_CHECK(convertYuv(&yuv420_bt601_image, yuv420_image.colorGamut, ULTRAHDR_COLORGAMUT_P3));
  }

  // compress 420 image
  JpegEncoderHelper jpeg_enc_obj_yuv420;
  if (!jpeg_enc_obj_yuv420.compressImage(
          reinterpret_cast<uint8_t*>(yuv420_bt601_image.data),
          reinterpret_cast<uint8_t*>(yuv420_bt601_image.chroma_data), yuv420_bt601_image.width,
          yuv420_bt601_image.height, yuv420_bt601_image.luma_stride,
          yuv420_bt601_image.chroma_stride, quality, icc->getData(), icc->getLength())) {
    return ERROR_ULTRAHDR_ENCODE_ERROR;
  }

  ultrahdr_compressed_struct jpeg;
  jpeg.data = jpeg_enc_obj_yuv420.getCompressedImagePtr();
  jpeg.length = static_cast<int>(jpeg_enc_obj_yuv420.getCompressedImageSize());
  jpeg.maxLength = static_cast<int>(jpeg_enc_obj_yuv420.getCompressedImageSize());
  jpeg.colorGamut = yuv420_image.colorGamut;

  // append gain map, no ICC since JPEG encode already did it
  ULTRAHDR_CHECK(appendGainMap(&jpeg, &compressed_map, exif, /* icc */ nullptr, /* icc size */ 0,
                            &metadata, dest));
  return ULTRAHDR_NO_ERROR;
}

/* Encode API-2 */
status_t JpegR::encodeJPEGR(uhdr_uncompressed_ptr p010_image_ptr,
                            uhdr_uncompressed_ptr yuv420_image_ptr,
                            uhdr_compressed_ptr yuv420jpg_image_ptr,
                            ultrahdr_transfer_function hdr_tf, uhdr_compressed_ptr dest) {
  // validate input arguments
  if (yuv420_image_ptr == nullptr) {
    ALOGE("received nullptr for uncompressed 420 image");
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (yuv420jpg_image_ptr == nullptr || yuv420jpg_image_ptr->data == nullptr) {
    ALOGE("received nullptr for compressed jpeg image");
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  ULTRAHDR_CHECK(areInputArgumentsValid(p010_image_ptr, yuv420_image_ptr, hdr_tf, dest))

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
    yuv420_image.chroma_data = data + yuv420_image.luma_stride * p010_image.height;
    yuv420_image.chroma_stride = yuv420_image.luma_stride >> 1;
  }

  // gain map
  ultrahdr_metadata_struct metadata;
  metadata.version = kGainMapVersion;
  ultrahdr_uncompressed_struct gainmap_image;
  ULTRAHDR_CHECK(generateGainMap(&yuv420_image, &p010_image, hdr_tf, &metadata, &gainmap_image));
  std::unique_ptr<uint8_t[]> map_data;
  map_data.reset(reinterpret_cast<uint8_t*>(gainmap_image.data));

  // compress gain map
  JpegEncoderHelper jpeg_enc_obj_gm;
  ULTRAHDR_CHECK(compressGainMap(&gainmap_image, &jpeg_enc_obj_gm));
  ultrahdr_compressed_struct gainmapjpg_image;
  gainmapjpg_image.data = jpeg_enc_obj_gm.getCompressedImagePtr();
  gainmapjpg_image.length = static_cast<int>(jpeg_enc_obj_gm.getCompressedImageSize());
  gainmapjpg_image.maxLength = static_cast<int>(jpeg_enc_obj_gm.getCompressedImageSize());
  gainmapjpg_image.colorGamut = ULTRAHDR_COLORGAMUT_UNSPECIFIED;

  return encodeJPEGR(yuv420jpg_image_ptr, &gainmapjpg_image, &metadata, dest);
}

/* Encode API-3 */
status_t JpegR::encodeJPEGR(uhdr_uncompressed_ptr p010_image_ptr,
                            uhdr_compressed_ptr yuv420jpg_image_ptr,
                            ultrahdr_transfer_function hdr_tf, uhdr_compressed_ptr dest) {
  // validate input arguments
  if (yuv420jpg_image_ptr == nullptr || yuv420jpg_image_ptr->data == nullptr) {
    ALOGE("received nullptr for compressed jpeg image");
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  ULTRAHDR_CHECK(areInputArgumentsValid(p010_image_ptr, nullptr, hdr_tf, dest))

  // clean up input structure for later usage
  ultrahdr_uncompressed_struct p010_image = *p010_image_ptr;
  if (p010_image.luma_stride == 0) p010_image.luma_stride = p010_image.width;
  if (!p010_image.chroma_data) {
    uint16_t* data = reinterpret_cast<uint16_t*>(p010_image.data);
    p010_image.chroma_data = data + p010_image.luma_stride * p010_image.height;
    p010_image.chroma_stride = p010_image.luma_stride;
  }

  // decode input jpeg, gamut is going to be bt601.
  JpegDecoderHelper jpeg_dec_obj_yuv420;
  if (!jpeg_dec_obj_yuv420.decompressImage(yuv420jpg_image_ptr->data,
                                           yuv420jpg_image_ptr->length)) {
    return ERROR_ULTRAHDR_DECODE_ERROR;
  }
  ultrahdr_uncompressed_struct yuv420_image{};
  yuv420_image.data = jpeg_dec_obj_yuv420.getDecompressedImagePtr();
  yuv420_image.width = jpeg_dec_obj_yuv420.getDecompressedImageWidth();
  yuv420_image.height = jpeg_dec_obj_yuv420.getDecompressedImageHeight();
  if (jpeg_dec_obj_yuv420.getICCSize() > 0) {
    ultrahdr_color_gamut cg = IccHelper::readIccColorGamut(jpeg_dec_obj_yuv420.getICCPtr(),
                                                           jpeg_dec_obj_yuv420.getICCSize());
    if (cg == ULTRAHDR_COLORGAMUT_UNSPECIFIED ||
        (yuv420jpg_image_ptr->colorGamut != ULTRAHDR_COLORGAMUT_UNSPECIFIED &&
         yuv420jpg_image_ptr->colorGamut != cg)) {
      ALOGE("configured color gamut  %d does not match with color gamut specified in icc box %d",
            yuv420jpg_image_ptr->colorGamut, cg);
      return ERROR_ULTRAHDR_INVALID_COLORGAMUT;
    }
    yuv420_image.colorGamut = cg;
  } else {
    if (yuv420jpg_image_ptr->colorGamut <= ULTRAHDR_COLORGAMUT_UNSPECIFIED ||
        yuv420jpg_image_ptr->colorGamut > ULTRAHDR_COLORGAMUT_MAX) {
      ALOGE("Unrecognized 420 color gamut %d", yuv420jpg_image_ptr->colorGamut);
      return ERROR_ULTRAHDR_INVALID_COLORGAMUT;
    }
    yuv420_image.colorGamut = yuv420jpg_image_ptr->colorGamut;
  }
  if (yuv420_image.luma_stride == 0) yuv420_image.luma_stride = yuv420_image.width;
  if (!yuv420_image.chroma_data) {
    uint8_t* data = reinterpret_cast<uint8_t*>(yuv420_image.data);
    yuv420_image.chroma_data = data + yuv420_image.luma_stride * p010_image.height;
    yuv420_image.chroma_stride = yuv420_image.luma_stride >> 1;
  }

  if (p010_image_ptr->width != yuv420_image.width ||
      p010_image_ptr->height != yuv420_image.height) {
    return ERROR_ULTRAHDR_RESOLUTION_MISMATCH;
  }

  // gain map
  ultrahdr_metadata_struct metadata;
  metadata.version = kGainMapVersion;
  ultrahdr_uncompressed_struct gainmap_image;
  ULTRAHDR_CHECK(generateGainMap(&yuv420_image, &p010_image, hdr_tf, &metadata, &gainmap_image,
                              true /* sdr_is_601 */));
  std::unique_ptr<uint8_t[]> map_data;
  map_data.reset(reinterpret_cast<uint8_t*>(gainmap_image.data));

  // compress gain map
  JpegEncoderHelper jpeg_enc_obj_gm;
  ULTRAHDR_CHECK(compressGainMap(&gainmap_image, &jpeg_enc_obj_gm));
  ultrahdr_compressed_struct gainmapjpg_image;
  gainmapjpg_image.data = jpeg_enc_obj_gm.getCompressedImagePtr();
  gainmapjpg_image.length = static_cast<int>(jpeg_enc_obj_gm.getCompressedImageSize());
  gainmapjpg_image.maxLength = static_cast<int>(jpeg_enc_obj_gm.getCompressedImageSize());
  gainmapjpg_image.colorGamut = ULTRAHDR_COLORGAMUT_UNSPECIFIED;

  return encodeJPEGR(yuv420jpg_image_ptr, &gainmapjpg_image, &metadata, dest);
}

/* Encode API-4 */
status_t JpegR::encodeJPEGR(uhdr_compressed_ptr yuv420jpg_image_ptr,
                            uhdr_compressed_ptr gainmapjpg_image_ptr, ultrahdr_metadata_ptr metadata,
                            uhdr_compressed_ptr dest) {
  if (yuv420jpg_image_ptr == nullptr || yuv420jpg_image_ptr->data == nullptr) {
    ALOGE("received nullptr for compressed jpeg image");
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (gainmapjpg_image_ptr == nullptr || gainmapjpg_image_ptr->data == nullptr) {
    ALOGE("received nullptr for compressed gain map");
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (dest == nullptr || dest->data == nullptr) {
    ALOGE("received nullptr for destination");
    return ERROR_ULTRAHDR_BAD_PTR;
  }

  // We just want to check if ICC is present, so don't do a full decode. Note,
  // this doesn't verify that the ICC is valid.
  JpegDecoderHelper decoder;
  if (!decoder.getCompressedImageParameters(yuv420jpg_image_ptr->data,
                                            yuv420jpg_image_ptr->length)) {
    return ERROR_ULTRAHDR_DECODE_ERROR;
  }

  // Add ICC if not already present.
  if (decoder.getICCSize() > 0) {
    ULTRAHDR_CHECK(appendGainMap(yuv420jpg_image_ptr, gainmapjpg_image_ptr, /* exif */ nullptr,
                              /* icc */ nullptr, /* icc size */ 0, metadata, dest));
  } else {
    if (yuv420jpg_image_ptr->colorGamut <= ULTRAHDR_COLORGAMUT_UNSPECIFIED ||
        yuv420jpg_image_ptr->colorGamut > ULTRAHDR_COLORGAMUT_MAX) {
      ALOGE("Unrecognized 420 color gamut %d", yuv420jpg_image_ptr->colorGamut);
      return ERROR_ULTRAHDR_INVALID_COLORGAMUT;
    }
    std::shared_ptr<DataStruct> newIcc =
        IccHelper::writeIccProfile(ULTRAHDR_TF_SRGB, yuv420jpg_image_ptr->colorGamut);
    ULTRAHDR_CHECK(appendGainMap(yuv420jpg_image_ptr, gainmapjpg_image_ptr, /* exif */ nullptr,
                              newIcc->getData(), newIcc->getLength(), metadata, dest));
  }

  return ULTRAHDR_NO_ERROR;
}

status_t JpegR::getJPEGRInfo(uhdr_compressed_ptr ultrahdr_image_ptr, uhdr_info_ptr ultrahdr_image_info_ptr) {
  if (ultrahdr_image_ptr == nullptr || ultrahdr_image_ptr->data == nullptr) {
    ALOGE("received nullptr for compressed jpegr image");
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (ultrahdr_image_info_ptr == nullptr) {
    ALOGE("received nullptr for compressed jpegr info struct");
    return ERROR_ULTRAHDR_BAD_PTR;
  }

  ultrahdr_compressed_struct primary_image, gainmap_image;
  status_t status = extractPrimaryImageAndGainMap(ultrahdr_image_ptr, &primary_image, &gainmap_image);
  if (status != ULTRAHDR_NO_ERROR) {
    return status;
  }
  status = parseJpegInfo(&primary_image, ultrahdr_image_info_ptr->primaryImgInfo,
                         &ultrahdr_image_info_ptr->width, &ultrahdr_image_info_ptr->height);
  if (status != ULTRAHDR_NO_ERROR) {
    return status;
  }
  if (ultrahdr_image_info_ptr->gainmapImgInfo != nullptr) {
    status = parseJpegInfo(&gainmap_image, ultrahdr_image_info_ptr->gainmapImgInfo);
    if (status != ULTRAHDR_NO_ERROR) {
      return status;
    }
  }

  return status;
}

/* Decode API */
status_t JpegR::decodeJPEGR(uhdr_compressed_ptr ultrahdr_image_ptr, uhdr_uncompressed_ptr dest,
                            float max_display_boost, uhdr_exif_ptr exif,
                            ultrahdr_output_format output_format,
                            uhdr_uncompressed_ptr gainmap_image_ptr, ultrahdr_metadata_ptr metadata) {
  if (ultrahdr_image_ptr == nullptr || ultrahdr_image_ptr->data == nullptr) {
    ALOGE("received nullptr for compressed jpegr image");
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (dest == nullptr || dest->data == nullptr) {
    ALOGE("received nullptr for dest image");
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (max_display_boost < 1.0f) {
    ALOGE("received bad value for max_display_boost %f", max_display_boost);
    return ERROR_ULTRAHDR_INVALID_DISPLAY_BOOST;
  }
  if (exif != nullptr && exif->data == nullptr) {
    ALOGE("received nullptr address for exif data");
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (gainmap_image_ptr != nullptr && gainmap_image_ptr->data == nullptr) {
    ALOGE("received nullptr address for gainmap data");
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (output_format <= ULTRAHDR_OUTPUT_UNSPECIFIED || output_format > ULTRAHDR_OUTPUT_MAX) {
    ALOGE("received bad value for output format %d", output_format);
    return ERROR_ULTRAHDR_INVALID_OUTPUT_FORMAT;
  }

  ultrahdr_compressed_struct primary_jpeg_image, gainmap_jpeg_image;
  status_t status =
      extractPrimaryImageAndGainMap(ultrahdr_image_ptr, &primary_jpeg_image, &gainmap_jpeg_image);
  if (status != ULTRAHDR_NO_ERROR) {
    ALOGE("received invalid compressed jpegr image");
    return status;
  }

  JpegDecoderHelper jpeg_dec_obj_yuv420;
  if (!jpeg_dec_obj_yuv420.decompressImage(
          primary_jpeg_image.data, primary_jpeg_image.length,
          (output_format == ULTRAHDR_OUTPUT_SDR) ? DECODE_TO_RGBA : DECODE_TO_YCBCR)) {
    return ERROR_ULTRAHDR_DECODE_ERROR;
  }

  if (output_format == ULTRAHDR_OUTPUT_SDR) {
#ifdef JCS_ALPHA_EXTENSIONS
    if ((jpeg_dec_obj_yuv420.getDecompressedImageWidth() *
         jpeg_dec_obj_yuv420.getDecompressedImageHeight() * 4) >
        jpeg_dec_obj_yuv420.getDecompressedImageSize()) {
      return ERROR_ULTRAHDR_DECODE_ERROR;
    }
#else
    if ((jpeg_dec_obj_yuv420.getDecompressedImageWidth() *
         jpeg_dec_obj_yuv420.getDecompressedImageHeight() * 3) >
        jpeg_dec_obj_yuv420.getDecompressedImageSize()) {
      return ERROR_ULTRAHDR_DECODE_ERROR;
    }
#endif
  } else {
    if ((jpeg_dec_obj_yuv420.getDecompressedImageWidth() *
         jpeg_dec_obj_yuv420.getDecompressedImageHeight() * 3 / 2) >
        jpeg_dec_obj_yuv420.getDecompressedImageSize()) {
      return ERROR_ULTRAHDR_DECODE_ERROR;
    }
  }

  if (exif != nullptr) {
    if (exif->length < jpeg_dec_obj_yuv420.getEXIFSize()) {
      return ERROR_ULTRAHDR_BUFFER_TOO_SMALL;
    }
    memcpy(exif->data, jpeg_dec_obj_yuv420.getEXIFPtr(), jpeg_dec_obj_yuv420.getEXIFSize());
    exif->length = jpeg_dec_obj_yuv420.getEXIFSize();
  }

  JpegDecoderHelper jpeg_dec_obj_gm;
  ultrahdr_uncompressed_struct gainmap_image;
  if (gainmap_image_ptr != nullptr || output_format != ULTRAHDR_OUTPUT_SDR) {
    if (!jpeg_dec_obj_gm.decompressImage(gainmap_jpeg_image.data, gainmap_jpeg_image.length)) {
      return ERROR_ULTRAHDR_DECODE_ERROR;
    }
    if ((jpeg_dec_obj_gm.getDecompressedImageWidth() *
         jpeg_dec_obj_gm.getDecompressedImageHeight()) >
        jpeg_dec_obj_gm.getDecompressedImageSize()) {
      return ERROR_ULTRAHDR_DECODE_ERROR;
    }
    gainmap_image.data = jpeg_dec_obj_gm.getDecompressedImagePtr();
    gainmap_image.width = jpeg_dec_obj_gm.getDecompressedImageWidth();
    gainmap_image.height = jpeg_dec_obj_gm.getDecompressedImageHeight();

    if (gainmap_image_ptr != nullptr) {
      gainmap_image_ptr->width = gainmap_image.width;
      gainmap_image_ptr->height = gainmap_image.height;
      memcpy(gainmap_image_ptr->data, gainmap_image.data,
             gainmap_image_ptr->width * gainmap_image_ptr->height);
    }
  }

  ultrahdr_metadata_struct uhdr_metadata;
  if (metadata != nullptr || output_format != ULTRAHDR_OUTPUT_SDR) {
    if (!getMetadataFromXMP(static_cast<uint8_t*>(jpeg_dec_obj_gm.getXMPPtr()),
                            jpeg_dec_obj_gm.getXMPSize(), &uhdr_metadata)) {
      return ERROR_ULTRAHDR_METADATA_ERROR;
    }
    if (metadata != nullptr) {
      metadata->version = uhdr_metadata.version;
      metadata->minContentBoost = uhdr_metadata.minContentBoost;
      metadata->maxContentBoost = uhdr_metadata.maxContentBoost;
      metadata->gamma = uhdr_metadata.gamma;
      metadata->offsetSdr = uhdr_metadata.offsetSdr;
      metadata->offsetHdr = uhdr_metadata.offsetHdr;
      metadata->hdrCapacityMin = uhdr_metadata.hdrCapacityMin;
      metadata->hdrCapacityMax = uhdr_metadata.hdrCapacityMax;
    }
  }

  if (output_format == ULTRAHDR_OUTPUT_SDR) {
    dest->width = jpeg_dec_obj_yuv420.getDecompressedImageWidth();
    dest->height = jpeg_dec_obj_yuv420.getDecompressedImageHeight();
#ifdef JCS_ALPHA_EXTENSIONS
    memcpy(dest->data, jpeg_dec_obj_yuv420.getDecompressedImagePtr(),
           dest->width * dest->height * 4);
#else
    uint32_t* pixelDst = static_cast<uint32_t*>(dest->data);
    uint8_t* pixelSrc = static_cast<uint8_t*>(jpeg_dec_obj_yuv420.getDecompressedImagePtr());
    for (int i = 0; i < dest->width * dest->height; i++) {
      *pixelDst = pixelSrc[0] | (pixelSrc[1] << 8) | (pixelSrc[2] << 16) | (0xff << 24);
      pixelSrc += 3;
      pixelDst += 1;
    }
#endif
    dest->colorGamut = IccHelper::readIccColorGamut(jpeg_dec_obj_yuv420.getICCPtr(),
                                                    jpeg_dec_obj_yuv420.getICCSize());
    return ULTRAHDR_NO_ERROR;
  }

  ultrahdr_uncompressed_struct yuv420_image;
  yuv420_image.data = jpeg_dec_obj_yuv420.getDecompressedImagePtr();
  yuv420_image.width = jpeg_dec_obj_yuv420.getDecompressedImageWidth();
  yuv420_image.height = jpeg_dec_obj_yuv420.getDecompressedImageHeight();
  yuv420_image.colorGamut = IccHelper::readIccColorGamut(jpeg_dec_obj_yuv420.getICCPtr(),
                                                         jpeg_dec_obj_yuv420.getICCSize());
  yuv420_image.luma_stride = yuv420_image.width;
  uint8_t* data = reinterpret_cast<uint8_t*>(yuv420_image.data);
  yuv420_image.chroma_data = data + yuv420_image.luma_stride * yuv420_image.height;
  yuv420_image.chroma_stride = yuv420_image.width >> 1;

  ULTRAHDR_CHECK(applyGainMap(&yuv420_image, &gainmap_image, &uhdr_metadata, output_format,
                           max_display_boost, dest));
  return ULTRAHDR_NO_ERROR;
}

status_t JpegR::compressGainMap(uhdr_uncompressed_ptr gainmap_image_ptr,
                                JpegEncoderHelper* jpeg_enc_obj_ptr) {
  if (gainmap_image_ptr == nullptr || jpeg_enc_obj_ptr == nullptr) {
    return ERROR_ULTRAHDR_BAD_PTR;
  }

  // Don't need to convert YUV to Bt601 since single channel
  if (!jpeg_enc_obj_ptr->compressImage(reinterpret_cast<uint8_t*>(gainmap_image_ptr->data), nullptr,
                                       gainmap_image_ptr->width, gainmap_image_ptr->height,
                                       gainmap_image_ptr->luma_stride, 0, kMapCompressQuality,
                                       nullptr, 0)) {
    return ERROR_ULTRAHDR_ENCODE_ERROR;
  }

  return ULTRAHDR_NO_ERROR;
}

status_t JpegR::extractPrimaryImageAndGainMap(uhdr_compressed_ptr ultrahdr_image_ptr,
                                              uhdr_compressed_ptr primary_jpg_image_ptr,
                                              uhdr_compressed_ptr gainmap_jpg_image_ptr) {
  if (ultrahdr_image_ptr == nullptr) {
    return ERROR_ULTRAHDR_BAD_PTR;
  }

  MessageHandler msg_handler;
  msg_handler.SetMessageWriter(make_unique<AlogMessageWriter>(AlogMessageWriter()));
  std::shared_ptr<DataSegment> seg = DataSegment::Create(
      DataRange(0, ultrahdr_image_ptr->length), static_cast<const uint8_t*>(ultrahdr_image_ptr->data),
      DataSegment::BufferDispositionPolicy::kDontDelete);
  DataSegmentDataSource data_source(seg);
  JpegInfoBuilder jpeg_info_builder;
  jpeg_info_builder.SetImageLimit(2);
  JpegScanner jpeg_scanner(&msg_handler);
  jpeg_scanner.Run(&data_source, &jpeg_info_builder);
  data_source.Reset();

  if (jpeg_scanner.HasError()) {
    return ULTRAHDR_UNKNOWN_ERROR;
  }

  const auto& jpeg_info = jpeg_info_builder.GetInfo();
  const auto& image_ranges = jpeg_info.GetImageRanges();

  if (image_ranges.empty()) {
    return ERROR_ULTRAHDR_NO_IMAGES_FOUND;
  }

  if (primary_jpg_image_ptr != nullptr) {
    primary_jpg_image_ptr->data =
        static_cast<uint8_t*>(ultrahdr_image_ptr->data) + image_ranges[0].GetBegin();
    primary_jpg_image_ptr->length = image_ranges[0].GetLength();
  }

  if (image_ranges.size() == 1) {
    return ERROR_ULTRAHDR_GAIN_MAP_IMAGE_NOT_FOUND;
  }

  if (gainmap_jpg_image_ptr != nullptr) {
    gainmap_jpg_image_ptr->data =
        static_cast<uint8_t*>(ultrahdr_image_ptr->data) + image_ranges[1].GetBegin();
    gainmap_jpg_image_ptr->length = image_ranges[1].GetLength();
  }

  // TODO: choose primary image and gain map image carefully
  if (image_ranges.size() > 2) {
    ALOGW("Number of jpeg images present %d, primary, gain map images may not be correctly chosen",
          (int)image_ranges.size());
  }

  return ULTRAHDR_NO_ERROR;
}

status_t JpegR::parseJpegInfo(uhdr_compressed_ptr jpeg_image_ptr, j_info_ptr jpeg_image_info_ptr,
                              size_t* img_width, size_t* img_height) {
  JpegDecoderHelper jpeg_dec_obj;
  if (!jpeg_dec_obj.getCompressedImageParameters(jpeg_image_ptr->data, jpeg_image_ptr->length)) {
    return ERROR_ULTRAHDR_DECODE_ERROR;
  }
  size_t imgWidth, imgHeight;
  imgWidth = jpeg_dec_obj.getDecompressedImageWidth();
  imgHeight = jpeg_dec_obj.getDecompressedImageHeight();

  if (jpeg_image_info_ptr != nullptr) {
    jpeg_image_info_ptr->width = imgWidth;
    jpeg_image_info_ptr->height = imgHeight;
    jpeg_image_info_ptr->imgData.resize(jpeg_image_ptr->length, 0);
    memcpy(static_cast<void*>(jpeg_image_info_ptr->imgData.data()), jpeg_image_ptr->data,
           jpeg_image_ptr->length);
    if (jpeg_dec_obj.getICCSize() != 0) {
      jpeg_image_info_ptr->iccData.resize(jpeg_dec_obj.getICCSize(), 0);
      memcpy(static_cast<void*>(jpeg_image_info_ptr->iccData.data()), jpeg_dec_obj.getICCPtr(),
             jpeg_dec_obj.getICCSize());
    }
    if (jpeg_dec_obj.getEXIFSize() != 0) {
      jpeg_image_info_ptr->exifData.resize(jpeg_dec_obj.getEXIFSize(), 0);
      memcpy(static_cast<void*>(jpeg_image_info_ptr->exifData.data()), jpeg_dec_obj.getEXIFPtr(),
             jpeg_dec_obj.getEXIFSize());
    }
    if (jpeg_dec_obj.getXMPSize() != 0) {
      jpeg_image_info_ptr->xmpData.resize(jpeg_dec_obj.getXMPSize(), 0);
      memcpy(static_cast<void*>(jpeg_image_info_ptr->xmpData.data()), jpeg_dec_obj.getXMPPtr(),
             jpeg_dec_obj.getXMPSize());
    }
  }
  if (img_width != nullptr && img_height != nullptr) {
    *img_width = imgWidth;
    *img_height = imgHeight;
  }
  return ULTRAHDR_NO_ERROR;
}

// JPEG/R structure:
// SOI (ff d8)
//
// (Optional, if EXIF package is from outside (Encode API-0 API-1), or if EXIF package presents
// in the JPEG input (Encode API-2, API-3, API-4))
// APP1 (ff e1)
// 2 bytes of length (2 + length of exif package)
// EXIF package (this includes the first two bytes representing the package length)
//
// (Required, XMP package) APP1 (ff e1)
// 2 bytes of length (2 + 29 + length of xmp package)
// name space ("http://ns.adobe.com/xap/1.0/\0")
// XMP
//
// (Required, MPF package) APP2 (ff e2)
// 2 bytes of length
// MPF
//
// (Required) primary image (without the first two bytes (SOI) and EXIF, may have other packages)
//
// SOI (ff d8)
//
// (Required, XMP package) APP1 (ff e1)
// 2 bytes of length (2 + 29 + length of xmp package)
// name space ("http://ns.adobe.com/xap/1.0/\0")
// XMP
//
// (Required) secondary image (the gain map, without the first two bytes (SOI))
//
// Metadata versions we are using:
// ECMA TR-98 for JFIF marker
// Exif 2.2 spec for EXIF marker
// Adobe XMP spec part 3 for XMP marker
// ICC v4.3 spec for ICC
status_t JpegR::appendGainMap(uhdr_compressed_ptr primary_jpg_image_ptr,
                              uhdr_compressed_ptr gainmap_jpg_image_ptr, uhdr_exif_ptr pExif,
                              void* pIcc, size_t icc_size, ultrahdr_metadata_ptr metadata,
                              uhdr_compressed_ptr dest) {
  if (primary_jpg_image_ptr == nullptr || gainmap_jpg_image_ptr == nullptr || metadata == nullptr ||
      dest == nullptr) {
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (metadata->version.compare("1.0")) {
    ALOGE("received bad value for version: %s", metadata->version.c_str());
    return ERROR_ULTRAHDR_BAD_METADATA;
  }
  if (metadata->maxContentBoost < metadata->minContentBoost) {
    ALOGE("received bad value for content boost min %f, max %f", metadata->minContentBoost,
          metadata->maxContentBoost);
    return ERROR_ULTRAHDR_BAD_METADATA;
  }
  if (metadata->hdrCapacityMax < metadata->hdrCapacityMin || metadata->hdrCapacityMin < 1.0f) {
    ALOGE("received bad value for hdr capacity min %f, max %f", metadata->hdrCapacityMin,
          metadata->hdrCapacityMax);
    return ERROR_ULTRAHDR_BAD_METADATA;
  }
  if (metadata->offsetSdr < 0.0f || metadata->offsetHdr < 0.0f) {
    ALOGE("received bad value for offset sdr %f, hdr %f", metadata->offsetSdr, metadata->offsetHdr);
    return ERROR_ULTRAHDR_BAD_METADATA;
  }
  if (metadata->gamma <= 0.0f) {
    ALOGE("received bad value for gamma %f", metadata->gamma);
    return ERROR_ULTRAHDR_BAD_METADATA;
  }

  const string nameSpace = "http://ns.adobe.com/xap/1.0/";
  const int nameSpaceLength = nameSpace.size() + 1;  // need to count the null terminator

  // calculate secondary image length first, because the length will be written into the primary
  // image xmp
  const string xmp_secondary = generateXmpForSecondaryImage(*metadata);
  // xmp_secondary_length = 2 bytes representing the length of the package +
  //  + nameSpaceLength = 29 bytes length
  //  + length of xmp packet = xmp_secondary.size()
  const int xmp_secondary_length = 2 + nameSpaceLength + xmp_secondary.size();
  const int secondary_image_size = 2 /* 2 bytes length of APP1 sign */
                                   + xmp_secondary_length + gainmap_jpg_image_ptr->length;
  // primary image
  const string xmp_primary = generateXmpForPrimaryImage(secondary_image_size, *metadata);
  // same as primary
  const int xmp_primary_length = 2 + nameSpaceLength + xmp_primary.size();

  // Check if EXIF package presents in the JPEG input.
  // If so, extract and remove the EXIF package.
  JpegDecoderHelper decoder;
  if (!decoder.extractEXIF(primary_jpg_image_ptr->data, primary_jpg_image_ptr->length)) {
    return ERROR_ULTRAHDR_DECODE_ERROR;
  }
  ultrahdr_exif_struct exif_from_jpg;
  exif_from_jpg.data = nullptr;
  exif_from_jpg.length = 0;
  ultrahdr_compressed_struct new_jpg_image;
  new_jpg_image.data = nullptr;
  new_jpg_image.length = 0;
  new_jpg_image.maxLength = 0;
  new_jpg_image.colorGamut = ULTRAHDR_COLORGAMUT_UNSPECIFIED;
  std::unique_ptr<uint8_t[]> dest_data;
  if (decoder.getEXIFPos() >= 0) {
    if (pExif != nullptr) {
      ALOGE("received EXIF from outside while the primary image already contains EXIF");
      return ERROR_ULTRAHDR_MULTIPLE_EXIFS_RECEIVED;
    }
    copyJpegWithoutExif(&new_jpg_image, primary_jpg_image_ptr, decoder.getEXIFPos(),
                        decoder.getEXIFSize());
    dest_data.reset(reinterpret_cast<uint8_t*>(new_jpg_image.data));
    exif_from_jpg.data = decoder.getEXIFPtr();
    exif_from_jpg.length = decoder.getEXIFSize();
    pExif = &exif_from_jpg;
  }

  uhdr_compressed_ptr final_primary_jpg_image_ptr =
      new_jpg_image.length == 0 ? primary_jpg_image_ptr : &new_jpg_image;

  int pos = 0;
  // Begin primary image
  // Write SOI
  ULTRAHDR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kStart, 1, pos));
  ULTRAHDR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kSOI, 1, pos));

  // Write EXIF
  if (pExif != nullptr) {
    const int length = 2 + pExif->length;
    const uint8_t lengthH = ((length >> 8) & 0xff);
    const uint8_t lengthL = (length & 0xff);
    ULTRAHDR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kStart, 1, pos));
    ULTRAHDR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kAPP1, 1, pos));
    ULTRAHDR_CHECK(Write(dest, &lengthH, 1, pos));
    ULTRAHDR_CHECK(Write(dest, &lengthL, 1, pos));
    ULTRAHDR_CHECK(Write(dest, pExif->data, pExif->length, pos));
  }

  // Prepare and write XMP
  {
    const int length = xmp_primary_length;
    const uint8_t lengthH = ((length >> 8) & 0xff);
    const uint8_t lengthL = (length & 0xff);
    ULTRAHDR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kStart, 1, pos));
    ULTRAHDR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kAPP1, 1, pos));
    ULTRAHDR_CHECK(Write(dest, &lengthH, 1, pos));
    ULTRAHDR_CHECK(Write(dest, &lengthL, 1, pos));
    ULTRAHDR_CHECK(Write(dest, (void*)nameSpace.c_str(), nameSpaceLength, pos));
    ULTRAHDR_CHECK(Write(dest, (void*)xmp_primary.c_str(), xmp_primary.size(), pos));
  }

  // Write ICC
  if (pIcc != nullptr && icc_size > 0) {
    const int length = icc_size + 2;
    const uint8_t lengthH = ((length >> 8) & 0xff);
    const uint8_t lengthL = (length & 0xff);
    ULTRAHDR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kStart, 1, pos));
    ULTRAHDR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kAPP2, 1, pos));
    ULTRAHDR_CHECK(Write(dest, &lengthH, 1, pos));
    ULTRAHDR_CHECK(Write(dest, &lengthL, 1, pos));
    ULTRAHDR_CHECK(Write(dest, pIcc, icc_size, pos));
  }

  // Prepare and write MPF
  {
    const int length = 2 + calculateMpfSize();
    const uint8_t lengthH = ((length >> 8) & 0xff);
    const uint8_t lengthL = (length & 0xff);
    int primary_image_size = pos + length + final_primary_jpg_image_ptr->length;
    // between APP2 + package size + signature
    // ff e2 00 58 4d 50 46 00
    // 2 + 2 + 4 = 8 (bytes)
    // and ff d8 sign of the secondary image
    int secondary_image_offset = primary_image_size - pos - 8;
    std::shared_ptr<DataStruct> mpf = generateMpf(primary_image_size, 0, /* primary_image_offset */
                                                  secondary_image_size, secondary_image_offset);
    ULTRAHDR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kStart, 1, pos));
    ULTRAHDR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kAPP2, 1, pos));
    ULTRAHDR_CHECK(Write(dest, &lengthH, 1, pos));
    ULTRAHDR_CHECK(Write(dest, &lengthL, 1, pos));
    ULTRAHDR_CHECK(Write(dest, (void*)mpf->getData(), mpf->getLength(), pos));
  }

  // Write primary image
  ULTRAHDR_CHECK(Write(dest, (uint8_t*)final_primary_jpg_image_ptr->data + 2,
                    final_primary_jpg_image_ptr->length - 2, pos));
  // Finish primary image

  // Begin secondary image (gain map)
  // Write SOI
  ULTRAHDR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kStart, 1, pos));
  ULTRAHDR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kSOI, 1, pos));

  // Prepare and write XMP
  {
    const int length = xmp_secondary_length;
    const uint8_t lengthH = ((length >> 8) & 0xff);
    const uint8_t lengthL = (length & 0xff);
    ULTRAHDR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kStart, 1, pos));
    ULTRAHDR_CHECK(Write(dest, &photos_editing_formats::image_io::JpegMarker::kAPP1, 1, pos));
    ULTRAHDR_CHECK(Write(dest, &lengthH, 1, pos));
    ULTRAHDR_CHECK(Write(dest, &lengthL, 1, pos));
    ULTRAHDR_CHECK(Write(dest, (void*)nameSpace.c_str(), nameSpaceLength, pos));
    ULTRAHDR_CHECK(Write(dest, (void*)xmp_secondary.c_str(), xmp_secondary.size(), pos));
  }

  // Write secondary image
  ULTRAHDR_CHECK(Write(dest, (uint8_t*)gainmap_jpg_image_ptr->data + 2,
                    gainmap_jpg_image_ptr->length - 2, pos));

  // Set back length
  dest->length = pos;

  // Done!
  return ULTRAHDR_NO_ERROR;
}

status_t JpegR::convertYuv(uhdr_uncompressed_ptr image, ultrahdr_color_gamut src_encoding,
                           ultrahdr_color_gamut dest_encoding) {
  if (image == nullptr) {
    return ERROR_ULTRAHDR_BAD_PTR;
  }
  if (src_encoding == ULTRAHDR_COLORGAMUT_UNSPECIFIED ||
      dest_encoding == ULTRAHDR_COLORGAMUT_UNSPECIFIED) {
    return ERROR_ULTRAHDR_INVALID_COLORGAMUT;
  }

  ColorTransformFn conversionFn = nullptr;
  switch (src_encoding) {
    case ULTRAHDR_COLORGAMUT_BT709:
      switch (dest_encoding) {
        case ULTRAHDR_COLORGAMUT_BT709:
          return ULTRAHDR_NO_ERROR;
        case ULTRAHDR_COLORGAMUT_P3:
          conversionFn = yuv709To601;
          break;
        case ULTRAHDR_COLORGAMUT_BT2100:
          conversionFn = yuv709To2100;
          break;
        default:
          // Should be impossible to hit after input validation
          return ERROR_ULTRAHDR_INVALID_COLORGAMUT;
      }
      break;
    case ULTRAHDR_COLORGAMUT_P3:
      switch (dest_encoding) {
        case ULTRAHDR_COLORGAMUT_BT709:
          conversionFn = yuv601To709;
          break;
        case ULTRAHDR_COLORGAMUT_P3:
          return ULTRAHDR_NO_ERROR;
        case ULTRAHDR_COLORGAMUT_BT2100:
          conversionFn = yuv601To2100;
          break;
        default:
          // Should be impossible to hit after input validation
          return ERROR_ULTRAHDR_INVALID_COLORGAMUT;
      }
      break;
    case ULTRAHDR_COLORGAMUT_BT2100:
      switch (dest_encoding) {
        case ULTRAHDR_COLORGAMUT_BT709:
          conversionFn = yuv2100To709;
          break;
        case ULTRAHDR_COLORGAMUT_P3:
          conversionFn = yuv2100To601;
          break;
        case ULTRAHDR_COLORGAMUT_BT2100:
          return ULTRAHDR_NO_ERROR;
        default:
          // Should be impossible to hit after input validation
          return ERROR_ULTRAHDR_INVALID_COLORGAMUT;
      }
      break;
    default:
      // Should be impossible to hit after input validation
      return ERROR_ULTRAHDR_INVALID_COLORGAMUT;
  }

  if (conversionFn == nullptr) {
    // Should be impossible to hit after input validation
    return ERROR_ULTRAHDR_INVALID_COLORGAMUT;
  }

  for (size_t y = 0; y < image->height / 2; ++y) {
    for (size_t x = 0; x < image->width / 2; ++x) {
      transformYuv420(image, x, y, conversionFn);
    }
  }

  return ULTRAHDR_NO_ERROR;
}

}  // namespace ultrahdr
