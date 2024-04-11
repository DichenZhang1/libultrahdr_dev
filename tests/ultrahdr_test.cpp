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

#include <gtest/gtest.h>

#include <fstream>
#include <iostream>

#include "ultrahdr/ultrahdr.h"
#include "ultrahdr/editorhelper.h"

//#define DUMP_OUTPUT

#ifdef DUMP_OUTPUT
static bool writeFile(const char* filename, void*& result, int length) {
  std::ofstream ofd(filename, std::ios::binary);
  if (ofd.is_open()) {
    ofd.write(static_cast<char*>(result), length);
    return true;
  }
  std::cerr << "unable to write to file : " << filename << std::endl;
  return false;
}
#endif

namespace ultrahdr {

#ifdef __ANDROID__
#define ULTRAHDR_IMAGE "/data/local/tmp/sample_jpegr.jpeg"
#define YUV420_IMAGE "/data/local/tmp/raw_yuv420_image.yuv420"
#define P010_IMAGE "/data/local/tmp/raw_p010_image.p010"
#define HEICR_IMAGE "/data/local/tmp/sample_heicr.heic"
#define AVIF_10_BIT_IMAGE "/data/local/tmp/avif_yuv_420_10bit.avif"
#define HEIC_10_BIT_IMAGE "/data/local/tmp/heifimage_10bit.heic"
#else
#define ULTRAHDR_IMAGE "./data/sample_jpegr.jpeg"
#define YUV420_IMAGE "./data/raw_yuv420_image.yuv420"
#define P010_IMAGE "./data/raw_p010_image.p010"
#define HEICR_IMAGE "./data/sample_heicr.heic"
#define AVIF_10_BIT_IMAGE "./data/avif_yuv_420_10bit.avif"
#define HEIC_10_BIT_IMAGE "./data/heifimage_10bit.heic"
#endif
#define WIDTH 1280
#define HEIGHT 720

class UltraHdrTest : public testing::Test {
 public:
  struct Image {
    std::unique_ptr<uint8_t[]> buffer;
    size_t size;
  };
  UltraHdrTest();
  ~UltraHdrTest();
};

UltraHdrTest::UltraHdrTest() {}

UltraHdrTest::~UltraHdrTest() {}

static bool loadFile(const char filename[], UltraHdrTest::Image* result, int *out_size = nullptr) {
  std::ifstream ifd(filename, std::ios::binary | std::ios::ate);
  if (ifd.good()) {
    int size = ifd.tellg();
    ifd.seekg(0, std::ios::beg);
    result->buffer.reset(new uint8_t[size]);
    ifd.read(reinterpret_cast<char*>(result->buffer.get()), size);
    ifd.close();

    if (out_size != nullptr) {
      *out_size = size;
    }
    return true;
  }
  return false;
}

TEST_F(UltraHdrTest, testFlow1) {
  Image ultrahdr_img;
  int length = 0;
  if (!loadFile(ULTRAHDR_IMAGE, &ultrahdr_img, &length)) {
    FAIL() << "Load file " << ULTRAHDR_IMAGE << " failed";
  }

  ultrahdr_compressed_struct jpegr;
  jpegr.length = length;
  jpegr.maxLength = length;
  jpegr.data = new uint8_t[length];
  jpegr.colorGamut = ULTRAHDR_COLORGAMUT_P3;
  std::unique_ptr<uint8_t[]> ultrahdr_data;
  ultrahdr_data.reset(reinterpret_cast<uint8_t*>(jpegr.data));
  memcpy(jpegr.data, ultrahdr_img.buffer.get(), length);

  ultrahdr_compressed_struct* dest = new ultrahdr_compressed_struct();

  UltraHdr uHdr;
  //  std::shared_ptr<ultrahdr_compressed_struct> img = std::make_shared<ultrahdr_compressed_struct>(std::move(jpegr));
  EXPECT_TRUE(uHdr.addImage(&jpegr) == ULTRAHDR_NO_ERROR);

  ultrahdr_configuration configuration;
  configuration.outputCodec = ULTRAHDR_CODEC_JPEG;


//    std::unique_ptr<uint8_t[]> dest_data = std::make_unique<uint8_t[]>(length);
//    dest.data = dest_data.get();
  EXPECT_TRUE(uHdr.convert(&configuration, dest) == ULTRAHDR_NO_ERROR);

  ultrahdr_uncompressed_struct* gainmap = new ultrahdr_uncompressed_struct();
  EXPECT_TRUE(uHdr.getGainMap(gainmap) == ULTRAHDR_NO_ERROR);

#ifdef DUMP_OUTPUT
  writeFile("debug_flow1.jpg", dest->data, dest->length);
#endif
}

TEST_F(UltraHdrTest, testFlow2) {
  Image yuv420_img;
  int length = 0;
  if (!loadFile(YUV420_IMAGE, &yuv420_img, &length)) {
    FAIL() << "Load file " << YUV420_IMAGE << " failed";
  }

  ultrahdr_uncompressed_struct yuv420;

  yuv420.width = WIDTH;
  yuv420.height = HEIGHT;
  yuv420.colorGamut = ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT709;
  yuv420.pixelFormat = ULTRAHDR_PIX_FMT_YUV420;
  std::unique_ptr<uint8_t[]> yuv420_data;
  yuv420.data = new uint8_t[length];
  yuv420_data.reset(reinterpret_cast<uint8_t*>(yuv420.data));
  memcpy(yuv420.data, yuv420_img.buffer.get(), length);

  ultrahdr_compressed_struct *dest = new ultrahdr_compressed_struct();

  UltraHdr uHdr;
  EXPECT_TRUE(uHdr.addImage(&yuv420) == ULTRAHDR_NO_ERROR);

  ultrahdr_configuration configuration;
  configuration.outputCodec = ULTRAHDR_CODEC_JPEG;
  configuration.quality = 80;


//    std::unique_ptr<uint8_t[]> dest_data = std::make_unique<uint8_t[]>(WIDTH * HEIGHT);
//    dest->data = dest_data.get();
  EXPECT_TRUE(uHdr.convert(&configuration, dest) == ULTRAHDR_NO_ERROR);

#ifdef DUMP_OUTPUT
  writeFile("debug_flow2.jpg", dest->data, dest->length);
#endif
}

TEST_F(UltraHdrTest, testFlow3) {
  Image p010_img;
  int length = 0;
  if (!loadFile(P010_IMAGE, &p010_img, &length)) {
    FAIL() << "Load file " << P010_IMAGE << " failed";
  }

  ultrahdr_uncompressed_struct p010;

  p010.width = WIDTH;
  p010.height = HEIGHT;
  p010.colorGamut = ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT709;
  p010.pixelFormat = ULTRAHDR_PIX_FMT_P010;
  std::unique_ptr<uint8_t[]> p010_data;
  p010.data = new uint8_t[length];
  p010_data.reset(reinterpret_cast<uint8_t*>(p010.data));
  memcpy(p010.data, p010_img.buffer.get(), length);

  ultrahdr_compressed_struct *dest = new ultrahdr_compressed_struct();

  UltraHdr uHdr;
  EXPECT_TRUE(uHdr.addImage(&p010) == ULTRAHDR_NO_ERROR);

  ultrahdr_configuration configuration;
  configuration.outputCodec = ULTRAHDR_CODEC_JPEG_R;
  configuration.quality = 80;
  configuration.transferFunction = ULTRAHDR_TF_HLG;

  ultrahdr_mirror_effect mirrorEffect;
  mirrorEffect.mirror_dir = ULTRAHDR_MIRROR_VERTICAL;
  ultrahdr_rotate_effect rotateEffect;
  rotateEffect.clockwise_degree = 90;
  configuration.effects.push_back(&mirrorEffect);
  configuration.effects.push_back(&rotateEffect);

//    std::unique_ptr<uint8_t[]> dest_data = std::make_unique<uint8_t[]>(WIDTH * HEIGHT);
//    dest->data = dest_data.get();
  EXPECT_TRUE(uHdr.convert(&configuration, dest) == ULTRAHDR_NO_ERROR);


#ifdef DUMP_OUTPUT
  writeFile("debug_flow3.jpg", dest->data, dest->length);
#endif
}

TEST_F(UltraHdrTest, testFlow4) {
  Image heicr_img;
  int length = 0;
  if (!loadFile(HEICR_IMAGE, &heicr_img, &length)) {
    FAIL() << "Load file " << HEICR_IMAGE << " failed";
  }

  ultrahdr_compressed_struct heicr;
  heicr.length = length;
  heicr.maxLength = length;
  heicr.data = new uint8_t[length];
  heicr.colorGamut = ULTRAHDR_COLORGAMUT_P3;
  std::unique_ptr<uint8_t[]> heicr_data;
  heicr_data.reset(reinterpret_cast<uint8_t*>(heicr.data));
  memcpy(heicr.data, heicr_img.buffer.get(), length);

  ultrahdr_compressed_struct* dest = new ultrahdr_compressed_struct();

  UltraHdr uHdr;
  EXPECT_TRUE(uHdr.addImage(&heicr) == ULTRAHDR_NO_ERROR);

  ultrahdr_configuration configuration;
  configuration.outputCodec = ULTRAHDR_CODEC_AVIF_R;
  configuration.quality = 80;
  configuration.transferFunction = ULTRAHDR_TF_HLG;

  ultrahdr_mirror_effect mirrorEffect;
  mirrorEffect.mirror_dir = ULTRAHDR_MIRROR_VERTICAL;
  ultrahdr_rotate_effect rotateEffect;
  rotateEffect.clockwise_degree = 90;
  configuration.effects.push_back(&mirrorEffect);
  configuration.effects.push_back(&rotateEffect);

  EXPECT_TRUE(uHdr.convert(&configuration, dest) == ULTRAHDR_NO_ERROR);

#ifdef DUMP_OUTPUT
  writeFile("debug_flow4.avif", dest->data, dest->length);
#endif
}

TEST_F(UltraHdrTest, testFlow5) {
  Image heicr_img;
  int length = 0;
  if (!loadFile(HEICR_IMAGE, &heicr_img, &length)) {
    FAIL() << "Load file " << HEICR_IMAGE << " failed";
  }

  ultrahdr_compressed_struct heicr;
  heicr.length = length;
  heicr.maxLength = length;
  heicr.data = new uint8_t[length];
  heicr.colorGamut = ULTRAHDR_COLORGAMUT_P3;
  std::unique_ptr<uint8_t[]> heicr_data;
  heicr_data.reset(reinterpret_cast<uint8_t*>(heicr.data));
  memcpy(heicr.data, heicr_img.buffer.get(), length);

  ultrahdr_uncompressed_struct* dest = new ultrahdr_uncompressed_struct();

  UltraHdr uHdr;
  EXPECT_TRUE(uHdr.addImage(&heicr) == ULTRAHDR_NO_ERROR);

  ultrahdr_configuration configuration;
  configuration.outputCodec = ULTRAHDR_CODEC_RAW_PIXELS;
  configuration.transferFunction = ULTRAHDR_TF_HLG;
  configuration.pixelFormat = ULTRAHDR_PIX_FMT_RGBA1010102;
  configuration.maxDisplayBoost = 100000000.0;

  ultrahdr_mirror_effect mirrorEffect;
  mirrorEffect.mirror_dir = ULTRAHDR_MIRROR_VERTICAL;
  ultrahdr_rotate_effect rotateEffect;
  rotateEffect.clockwise_degree = 90;
  configuration.effects.push_back(&mirrorEffect);
  configuration.effects.push_back(&rotateEffect);

  EXPECT_TRUE(uHdr.convert(&configuration, dest) == ULTRAHDR_NO_ERROR);

#ifdef DUMP_OUTPUT
  writeFile("debug_flow5.rgb", dest->data, WIDTH * HEIGHT * 4);
#endif
}

TEST_F(UltraHdrTest, testFlow6) {
  Image avif_img;
  int length = 0;
  if (!loadFile(AVIF_10_BIT_IMAGE, &avif_img, &length)) {
    FAIL() << "Load file " << AVIF_10_BIT_IMAGE << " failed";
  }

  ultrahdr_compressed_struct avif;
  avif.length = length;
  avif.maxLength = length;
  avif.data = new uint8_t[length];
  avif.colorGamut = ULTRAHDR_COLORGAMUT_P3;
  std::unique_ptr<uint8_t[]> avif_data;
  avif_data.reset(reinterpret_cast<uint8_t*>(avif.data));
  memcpy(avif.data, avif_img.buffer.get(), length);

  ultrahdr_compressed_struct* dest = new ultrahdr_compressed_struct();

  UltraHdr uHdr;
  EXPECT_TRUE(uHdr.addImage(&avif) == ULTRAHDR_NO_ERROR);

  ultrahdr_configuration configuration;
  configuration.outputCodec = ULTRAHDR_CODEC_HEIC_R;
  configuration.transferFunction = ULTRAHDR_TF_HLG;
  configuration.quality = 80;

  ultrahdr_mirror_effect mirrorEffect;
  mirrorEffect.mirror_dir = ULTRAHDR_MIRROR_VERTICAL;
  ultrahdr_rotate_effect rotateEffect;
  rotateEffect.clockwise_degree = 90;
//  configuration.effects.push_back(&mirrorEffect);
//  configuration.effects.push_back(&rotateEffect);

  EXPECT_TRUE(uHdr.convert(&configuration, dest) == ULTRAHDR_NO_ERROR);

#ifdef DUMP_OUTPUT
  writeFile("debug_flow6.heic", dest->data, WIDTH * HEIGHT);
#endif
}

TEST_F(UltraHdrTest, testFlow7) {
  Image heicr_img;
  int length = 0;
  if (!loadFile(HEICR_IMAGE, &heicr_img, &length)) {
    FAIL() << "Load file " << HEICR_IMAGE << " failed";
  }

  ultrahdr_compressed_struct heicr;
  heicr.length = length;
  heicr.maxLength = length;
  heicr.data = new uint8_t[length];
  heicr.colorGamut = ULTRAHDR_COLORGAMUT_P3;
  std::unique_ptr<uint8_t[]> heicr_data;
  heicr_data.reset(reinterpret_cast<uint8_t*>(heicr.data));
  memcpy(heicr.data, heicr_img.buffer.get(), length);

  ultrahdr_compressed_struct* dest = new ultrahdr_compressed_struct();

  UltraHdr uHdr;
  EXPECT_TRUE(uHdr.addImage(&heicr) == ULTRAHDR_NO_ERROR);

  ultrahdr_configuration configuration;
  configuration.outputCodec = ULTRAHDR_CODEC_AVIF_10_BIT;
  configuration.transferFunction = ULTRAHDR_TF_HLG;
  configuration.maxDisplayBoost = 100000000.0;

  ultrahdr_mirror_effect mirrorEffect;
  mirrorEffect.mirror_dir = ULTRAHDR_MIRROR_VERTICAL;
  ultrahdr_rotate_effect rotateEffect;
  rotateEffect.clockwise_degree = 90;
  configuration.effects.push_back(&mirrorEffect);
  configuration.effects.push_back(&rotateEffect);

  EXPECT_TRUE(uHdr.convert(&configuration, dest) == ULTRAHDR_NO_ERROR);

#ifdef DUMP_OUTPUT
  writeFile("debug_flow7.avif", dest->data, dest->length);
#endif
}

}  // namespace ultrahdr
