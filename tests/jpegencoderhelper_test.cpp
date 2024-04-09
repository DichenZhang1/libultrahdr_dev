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

#include <gtest/gtest.h>

#include <fstream>
#include <iostream>

#include "ultrahdr/ultrahdr.h"
#include "ultrahdr/jpegencoderhelper.h"

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

class JpegEncoderHelperTest : public testing::Test {
 public:
  struct Image {
    std::unique_ptr<uint8_t[]> buffer;
    size_t width;
    size_t height;
  };
  JpegEncoderHelperTest();
  ~JpegEncoderHelperTest();

 protected:
  virtual void SetUp();
  virtual void TearDown();

  Image mAlignedImage, mUnalignedImage, mSingleChannelImage;
};

JpegEncoderHelperTest::JpegEncoderHelperTest() {}

JpegEncoderHelperTest::~JpegEncoderHelperTest() {}

static bool loadFile(const char filename[], JpegEncoderHelperTest::Image* result) {
  std::ifstream ifd(filename, std::ios::binary | std::ios::ate);
  if (ifd.good()) {
    int size = ifd.tellg();
    ifd.seekg(0, std::ios::beg);
    result->buffer.reset(new uint8_t[size]);
    ifd.read(reinterpret_cast<char*>(result->buffer.get()), size);
    ifd.close();
    return true;
  }
  return false;
}

void JpegEncoderHelperTest::SetUp() {
  if (!loadFile(ALIGNED_IMAGE, &mAlignedImage)) {
    FAIL() << "Load file " << ALIGNED_IMAGE << " failed";
  }
  mAlignedImage.width = ALIGNED_IMAGE_WIDTH;
  mAlignedImage.height = ALIGNED_IMAGE_HEIGHT;
  if (!loadFile(UNALIGNED_IMAGE, &mUnalignedImage)) {
    FAIL() << "Load file " << UNALIGNED_IMAGE << " failed";
  }
  mUnalignedImage.width = UNALIGNED_IMAGE_WIDTH;
  mUnalignedImage.height = UNALIGNED_IMAGE_HEIGHT;
  if (!loadFile(SINGLE_CHANNEL_IMAGE, &mSingleChannelImage)) {
    FAIL() << "Load file " << SINGLE_CHANNEL_IMAGE << " failed";
  }
  mSingleChannelImage.width = SINGLE_CHANNEL_IMAGE_WIDTH;
  mSingleChannelImage.height = SINGLE_CHANNEL_IMAGE_HEIGHT;
}

void JpegEncoderHelperTest::TearDown() {}

TEST_F(JpegEncoderHelperTest, encodeAlignedImage) {
  JpegEncoderHelper encoder;
  EXPECT_TRUE(encoder.compressImage(
      mAlignedImage.buffer.get(),
      mAlignedImage.buffer.get() + mAlignedImage.width * mAlignedImage.height, mAlignedImage.width,
      mAlignedImage.height, mAlignedImage.width, mAlignedImage.width / 2, JPEG_QUALITY, NULL, 0));
  ASSERT_GT(encoder.getCompressedImageSize(), static_cast<uint32_t>(0));
}

TEST_F(JpegEncoderHelperTest, encodeUnalignedImage) {
  JpegEncoderHelper encoder;
  EXPECT_TRUE(encoder.compressImage(
      mUnalignedImage.buffer.get(),
      mUnalignedImage.buffer.get() + mUnalignedImage.width * mUnalignedImage.height,
      mUnalignedImage.width, mUnalignedImage.height, mUnalignedImage.width,
      mUnalignedImage.width / 2, JPEG_QUALITY, NULL, 0));
  ASSERT_GT(encoder.getCompressedImageSize(), static_cast<uint32_t>(0));
}

TEST_F(JpegEncoderHelperTest, encodeSingleChannelImage) {
  JpegEncoderHelper encoder;
  EXPECT_TRUE(encoder.compressImage(mSingleChannelImage.buffer.get(), nullptr,
                                    mSingleChannelImage.width, mSingleChannelImage.height,
                                    mSingleChannelImage.width, 0, JPEG_QUALITY, NULL, 0));
  ASSERT_GT(encoder.getCompressedImageSize(), static_cast<uint32_t>(0));
}

}  // namespace ultrahdr
