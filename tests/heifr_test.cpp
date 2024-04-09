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

#include "ultrahdr/ultrahdrcommon.h"
#include "ultrahdr/ultrahdr.h"
#include "ultrahdr/heifr.h"

//#define DUMP_OUTPUT

namespace ultrahdr {

#ifdef __ANDROID__
#define ALIGNED_IMAGE "/data/local/tmp/minnie-320x240.yu12"
#define SINGLE_CHANNEL_IMAGE "/data/local/tmp/minnie-320x240.y"
#define UNALIGNED_IMAGE "/data/local/tmp/minnie-318x240.yu12"
#else
#define ALIGNED_IMAGE "./data/minnie-320x240.yu12"
#define SINGLE_CHANNEL_IMAGE "./data/minnie-320x240.y"
#define UNALIGNED_IMAGE "./data/minnie-318x240.yu12"
#endif
#define ALIGNED_IMAGE_WIDTH 320
#define ALIGNED_IMAGE_HEIGHT 240
#define SINGLE_CHANNEL_IMAGE_WIDTH ALIGNED_IMAGE_WIDTH
#define SINGLE_CHANNEL_IMAGE_HEIGHT ALIGNED_IMAGE_HEIGHT
#define UNALIGNED_IMAGE_WIDTH 318
#define UNALIGNED_IMAGE_HEIGHT 240


#define JPEG_QUALITY 90
#define P010_IMAGE "./data/raw_p010_image.p010"
#define YUV420_IMAGE "./data/raw_yuv420_image.yuv420"
#define WIDTH 1280
#define HEIGHT 720

class HeifWithGainMapTest : public testing::Test {
 public:
  struct Image {
    std::unique_ptr<uint8_t[]> buffer;
    size_t width;
    size_t height;
  };
  HeifWithGainMapTest();
  ~HeifWithGainMapTest();
};

HeifWithGainMapTest::HeifWithGainMapTest() {}

HeifWithGainMapTest::~HeifWithGainMapTest() {}

static bool loadFile(const char filename[], HeifWithGainMapTest::Image* result, int *out_size = nullptr) {
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

static bool writeFile(const char* filename, void*& result, int length) {
  std::ofstream ofd(filename, std::ios::binary);
  if (ofd.is_open()) {
    ofd.write(static_cast<char*>(result), length);
    return true;
  }
  std::cerr << "unable to write to file : " << filename << std::endl;
  return false;
}

TEST_F(HeifWithGainMapTest, encodeAPI0HeicTest) {
  Image p010_img;
  if (!loadFile(P010_IMAGE, &p010_img)) {
    FAIL() << "Load file " << P010_IMAGE << " failed";
  }

  ultrahdr_uncompressed_struct p010;
  p010.width = WIDTH;
  p010.height = HEIGHT;
  p010.colorGamut = ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100;
  p010.data = p010_img.buffer.get();

  HeifR encoder;
  ultrahdr_compressed_struct dest;
  dest.data = new uint8_t[WIDTH * HEIGHT];
  std::unique_ptr<uint8_t[]> dest_data;
  dest_data.reset(reinterpret_cast<uint8_t*>(dest.data));

  EXPECT_TRUE(encoder.encodeHeifWithGainMap(&p010, /* p010 */
                                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                            &dest,
                                            100, /* quality */
                                            ULTRAHDR_CODEC_HEIC_R,
                                            nullptr /* exif */ ) == ULTRAHDR_NO_ERROR);
  EXPECT_TRUE(dest.length > 0);

  HeifR decoder;
  ultrahdr_uncompressed_struct recon;
  recon.data = new uint8_t[WIDTH * HEIGHT * 8];
  std::unique_ptr<uint8_t[]> recon_data;
  recon_data.reset(reinterpret_cast<uint8_t*>(recon.data));
  EXPECT_TRUE(decoder.decodeHeifWithGainMap(&dest, &recon) == ULTRAHDR_NO_ERROR);
}

TEST_F(HeifWithGainMapTest, encodeAPI0AvifTest) {
  Image p010_img;
  if (!loadFile(P010_IMAGE, &p010_img)) {
    FAIL() << "Load file " << P010_IMAGE << " failed";
  }

  ultrahdr_uncompressed_struct p010;
  p010.width = WIDTH;
  p010.height = HEIGHT;
  p010.colorGamut = ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100;
  p010.data = p010_img.buffer.get();

  HeifR encoder;
  ultrahdr_compressed_struct dest;
  dest.data = new uint8_t[WIDTH * HEIGHT];
  std::unique_ptr<uint8_t[]> dest_data;
  dest_data.reset(reinterpret_cast<uint8_t*>(dest.data));

  HeifR avifEncoder;
  EXPECT_TRUE(encoder.encodeHeifWithGainMap(&p010, /* p010 */
                                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                            &dest,
                                            100, /* quality */
                                            ULTRAHDR_CODEC_AVIF_R,
                                            nullptr /* exif */ ) == ULTRAHDR_NO_ERROR);
  EXPECT_TRUE(dest.length > 0);

  HeifR decoder;
  ultrahdr_uncompressed_struct recon;
  recon.data = new uint8_t[WIDTH * HEIGHT * 8];
  std::unique_ptr<uint8_t[]> recon_data;
  recon_data.reset(reinterpret_cast<uint8_t*>(recon.data));
  EXPECT_TRUE(decoder.decodeHeifWithGainMap(&dest, &recon) == ULTRAHDR_NO_ERROR);
}

TEST_F(HeifWithGainMapTest, encodeAPI1HeicTest) {
  Image p010_img, yuv420_img;
  if (!loadFile(P010_IMAGE, &p010_img)) {
    FAIL() << "Load file " << P010_IMAGE << " failed";
  }
    if (!loadFile(YUV420_IMAGE, &yuv420_img)) {
    FAIL() << "Load file " << YUV420_IMAGE << " failed";
  }

  ultrahdr_uncompressed_struct p010;
  p010.width = WIDTH;
  p010.height = HEIGHT;
  p010.colorGamut = ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100;
  p010.data = p010_img.buffer.get();

  ultrahdr_uncompressed_struct yuv420;
  yuv420.width = WIDTH;
  yuv420.height = HEIGHT;
  yuv420.colorGamut = ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT709;
  yuv420.data = yuv420_img.buffer.get();

  HeifR encoder;
  ultrahdr_compressed_struct dest;
  dest.data = new uint8_t[WIDTH * HEIGHT];
  std::unique_ptr<uint8_t[]> dest_data;
  dest_data.reset(reinterpret_cast<uint8_t*>(dest.data));

  EXPECT_TRUE(encoder.encodeHeifWithGainMap(&p010, /* p010 */
                                            &yuv420, /* yuv_420 */
                                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                            &dest,
                                            100, /* quality */
                                            ULTRAHDR_CODEC_HEIC_R,
                                            nullptr /* exif */ ) == ULTRAHDR_NO_ERROR);
  EXPECT_TRUE(dest.length > 0);

  HeifR decoder;
  ultrahdr_uncompressed_struct recon;
  recon.data = new uint8_t[WIDTH * HEIGHT * 8];
  std::unique_ptr<uint8_t[]> recon_data;
  recon_data.reset(reinterpret_cast<uint8_t*>(recon.data));
  EXPECT_TRUE(decoder.decodeHeifWithGainMap(&dest, &recon) == ULTRAHDR_NO_ERROR);
}

TEST_F(HeifWithGainMapTest, encodeAPI1AvifTest) {
  Image p010_img, yuv420_img;
  if (!loadFile(P010_IMAGE, &p010_img)) {
    FAIL() << "Load file " << P010_IMAGE << " failed";
  }
    if (!loadFile(YUV420_IMAGE, &yuv420_img)) {
    FAIL() << "Load file " << YUV420_IMAGE << " failed";
  }

  ultrahdr_uncompressed_struct p010;
  p010.width = WIDTH;
  p010.height = HEIGHT;
  p010.colorGamut = ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT2100;
  p010.data = p010_img.buffer.get();

  ultrahdr_uncompressed_struct yuv420;
  yuv420.width = WIDTH;
  yuv420.height = HEIGHT;
  yuv420.colorGamut = ultrahdr_color_gamut::ULTRAHDR_COLORGAMUT_BT709;
  yuv420.data = yuv420_img.buffer.get();

  HeifR encoder;
  ultrahdr_compressed_struct dest;
  dest.data = new uint8_t[WIDTH * HEIGHT];
  std::unique_ptr<uint8_t[]> dest_data;
  dest_data.reset(reinterpret_cast<uint8_t*>(dest.data));

  EXPECT_TRUE(encoder.encodeHeifWithGainMap(&p010, /* p010 */
                                            &yuv420, /* yuv_420 */
                                            ultrahdr_transfer_function::ULTRAHDR_TF_HLG,
                                            &dest,
                                            100, /* quality */
                                            ULTRAHDR_CODEC_AVIF_R,
                                            nullptr /* exif */ ) == ULTRAHDR_NO_ERROR);
  EXPECT_TRUE(dest.length > 0);

  HeifR decoder;
  ultrahdr_uncompressed_struct recon;
  recon.data = new uint8_t[WIDTH * HEIGHT * 8];
  std::unique_ptr<uint8_t[]> recon_data;
  recon_data.reset(reinterpret_cast<uint8_t*>(recon.data));
  EXPECT_TRUE(decoder.decodeHeifWithGainMap(&dest, &recon) == ULTRAHDR_NO_ERROR);
}

}