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

#ifndef ULTRAHDR_EDITORHELPER_H
#define ULTRAHDR_EDITORHELPER_H

#include "ultrahdr/ultrahdr.h"
#include "ultrahdr/jpegr.h"

namespace ultrahdr {
typedef enum {
  ULTRAHDR_MIRROR_VERTICAL,
  ULTRAHDR_MIRROR_HORIZONTAL,
} ultrahdr_mirroring_direction;

typedef struct ultrahdr_crop_effect_struct : ultrahdr_effect {
  int left;
  int right;
  int top;
  int bottom;
} ultrahdr_crop_effect;

typedef struct ultrahdr_mirror_effect_struct : ultrahdr_effect {
  ultrahdr_mirroring_direction mirror_dir;
} ultrahdr_mirror_effect;

typedef struct ultrahdr_rotate_effect_struct : ultrahdr_effect {
  int clockwise_degree;
} ultrahdr_rotate_effect;

typedef struct ultrahdr_resize_effect_struct : ultrahdr_effect {
  int new_width;
  int new_height;
} ultrahdr_resize_effect;

status_t crop(uhdr_uncompressed_ptr const in_img,
              int left, int right, int top, int bottom, uhdr_uncompressed_ptr out_img);

status_t mirror(uhdr_uncompressed_ptr const in_img,
                ultrahdr_mirroring_direction mirror_dir,
                uhdr_uncompressed_ptr out_img);

status_t rotate(uhdr_uncompressed_ptr const in_img, int clockwise_degree,
                uhdr_uncompressed_ptr out_img);

status_t resize(uhdr_uncompressed_ptr const in_img, int out_width, int out_height,
                uhdr_uncompressed_ptr out_img);

status_t addEffects(uhdr_uncompressed_ptr const in_img, std::vector<ultrahdr_effect*>& effects,
                    uhdr_uncompressed_ptr out_image);

}  // namespace ultrahdr

#endif  // ULTRAHDR_EDITORHELPER_H
