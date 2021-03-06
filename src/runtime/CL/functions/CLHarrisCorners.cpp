/*
 * Copyright (c) 2016, 2017 ARM Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "arm_compute/runtime/CL/functions/CLHarrisCorners.h"

#include "arm_compute/core/CL/OpenCL.h"
#include "arm_compute/core/CL/kernels/CLFillBorderKernel.h"
#include "arm_compute/core/CL/kernels/CLHarrisCornersKernel.h"
#include "arm_compute/core/Error.h"
#include "arm_compute/core/Helpers.h"
#include "arm_compute/core/TensorInfo.h"
#include "arm_compute/core/Validate.h"
#include "arm_compute/runtime/CL/CLScheduler.h"
#include "arm_compute/runtime/CL/functions/CLSobel3x3.h"
#include "arm_compute/runtime/CL/functions/CLSobel5x5.h"
#include "arm_compute/runtime/CL/functions/CLSobel7x7.h"
#include "arm_compute/runtime/CPP/CPPScheduler.h"
#include "arm_compute/runtime/ITensorAllocator.h"

#include <cmath>
#include <utility>

using namespace arm_compute;

CLHarrisCorners::CLHarrisCorners()
    : _sobel(), _harris_score(), _non_max_suppr(), _candidates(), _sort_euclidean(), _border_gx(), _border_gy(), _gx(), _gy(), _score(), _nonmax(), _corners_list(), _num_corner_candidates(0),
      _corners(nullptr)
{
}

void CLHarrisCorners::configure(ICLImage *input, float threshold, float min_dist,
                                float sensitivity, int32_t gradient_size, int32_t block_size, ICLKeyPointArray *corners,
                                BorderMode border_mode, uint8_t constant_border_value)
{
    ARM_COMPUTE_ERROR_ON_TENSOR_NOT_2D(input);
    ARM_COMPUTE_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(input, 1, DataType::U8);
    ARM_COMPUTE_ERROR_ON(!(block_size == 3 || block_size == 5 || block_size == 7));
    ARM_COMPUTE_ERROR_ON(nullptr == corners);

    _corners = corners;

    const TensorShape shape = input->info()->tensor_shape();
    const DataType    dt    = (gradient_size < 7) ? DataType::S16 : DataType::S32;
    TensorInfo        tensor_info(shape, 1, dt);
    _gx.allocator()->init(tensor_info);
    _gy.allocator()->init(tensor_info);

    TensorInfo info_f32(shape, 1, DataType::F32);
    _score.allocator()->init(info_f32);
    _nonmax.allocator()->init(info_f32);
    _corners_list = arm_compute::cpp14::make_unique<InternalKeypoint[]>(shape.x() * shape.y());

    /* Set/init Sobel kernel accordingly with gradient_size */
    switch(gradient_size)
    {
        case 3:
        {
            auto k = arm_compute::cpp14::make_unique<CLSobel3x3>();
            k->configure(input, &_gx, &_gy, border_mode, constant_border_value);
            _sobel = std::move(k);
            break;
        }
        case 5:
        {
            auto k = arm_compute::cpp14::make_unique<CLSobel5x5>();
            k->configure(input, &_gx, &_gy, border_mode, constant_border_value);
            _sobel = std::move(k);
            break;
        }
        case 7:
        {
            auto k = arm_compute::cpp14::make_unique<CLSobel7x7>();
            k->configure(input, &_gx, &_gy, border_mode, constant_border_value);
            _sobel = std::move(k);
            break;
        }
        default:
            ARM_COMPUTE_ERROR("Gradient size not implemented");
    }

    // Configure border filling before harris score
    _border_gx.configure(&_gx, block_size / 2, border_mode, constant_border_value);
    _border_gy.configure(&_gy, block_size / 2, border_mode, constant_border_value);

    // Normalization factor
    const float norm_factor               = 1.0f / (255.0f * pow(4.0f, gradient_size / 2) * block_size);
    const float pow4_normalization_factor = pow(norm_factor, 4);

    // Set/init Harris Score kernel accordingly with block_size
    _harris_score.configure(&_gx, &_gy, &_score, block_size, pow4_normalization_factor, threshold, sensitivity, border_mode == BorderMode::UNDEFINED);

    // Init non-maxima suppression function
    _non_max_suppr.configure(&_score, &_nonmax, border_mode == BorderMode::UNDEFINED);

    // Init corner candidates kernel
    _candidates.configure(&_nonmax, _corners_list.get(), &_num_corner_candidates);

    // Init euclidean distance
    _sort_euclidean.configure(_corners_list.get(), _corners, &_num_corner_candidates, min_dist);

    // Allocate intermediate buffers
    _gx.allocator()->allocate();
    _gy.allocator()->allocate();
    _score.allocator()->allocate();
    _nonmax.allocator()->allocate();
}

void CLHarrisCorners::run()
{
    ARM_COMPUTE_ERROR_ON_MSG(_sobel == nullptr, "Unconfigured function");

    // Init to 0 number of corner candidates
    _num_corner_candidates = 0;

    // Run Sobel kernel
    _sobel->run();

    // Fill border before harris score kernel
    CLScheduler::get().enqueue(_border_gx, false);
    CLScheduler::get().enqueue(_border_gy, false);

    // Run harris score kernel
    CLScheduler::get().enqueue(_harris_score, false);

    // Run non-maxima suppression
    CLScheduler::get().enqueue(_non_max_suppr);

    // Run corner candidate kernel
    _nonmax.map(true);
    CPPScheduler::get().multithread(&_candidates);
    _nonmax.unmap();

    _corners->map(CLScheduler::get().queue(), true);
    _sort_euclidean.run(_sort_euclidean.window());
    _corners->unmap(CLScheduler::get().queue());
}
