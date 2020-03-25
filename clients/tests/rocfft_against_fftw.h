// Copyright (c) 2016 - present Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#pragma once
#ifndef ROCFFT_AGAINST_FFTW
#define ROCFFT_AGAINST_FFTW

#include <gtest/gtest.h>
#include <math.h>
#include <stdexcept>
#include <vector>

#include "fftw_transform.h"
#include "rocfft.h"
#include <hip/hip_runtime.h>

// Return the precision enum for rocFFT based upon the type.
template <typename Tfloat>
inline rocfft_precision precision_selector();
template <>
inline rocfft_precision precision_selector<float>()
{
    return rocfft_precision_single;
}
template <>
inline rocfft_precision precision_selector<double>()
{
    return rocfft_precision_double;
}

// Check if the required buffers fit in the device vram.
inline bool
    vram_fits_problem(const size_t isize, const size_t osize, const size_t wsize, int deviceId = 0)
{
    hipDeviceProp_t prop;
    auto            retval = hipGetDeviceProperties(&prop, deviceId);
    assert(retval == hipSuccess);
    return prop.totalGlobalMem > isize + osize + wsize;
}

// Perform and out-of-place computation on contiguous data and then return this in an
// an object which will destruct correctly.
template <typename Tfloat, typename Tallocator>
inline std::vector<std::vector<char, Tallocator>>
    fftw_transform(const std::vector<fftw_iodim64> dims,
                   const std::vector<fftw_iodim64> howmany_dims,
                   const rocfft_transform_type     transformType,
                   const size_t                    osize,
                   void*                           cpu_in)
{
    fftw_initiate_threads<Tfloat>();
    typename fftw_trait<Tfloat>::fftw_plan_type cpu_plan = NULL;
    using fftw_complex_type = typename fftw_trait<Tfloat>::fftw_complex_type;

    std::vector<std::vector<char, Tallocator>> output(1);
    switch(transformType)
    {
    case rocfft_transform_type_complex_forward:
        output[0].resize(osize * sizeof(fftw_complex_type));
        cpu_plan
            = fftw_plan_guru64_dft<Tfloat>(dims.size(),
                                           dims.data(),
                                           howmany_dims.size(),
                                           howmany_dims.data(),
                                           reinterpret_cast<fftw_complex_type*>(cpu_in),
                                           reinterpret_cast<fftw_complex_type*>(output[0].data()),
                                           -1,
                                           FFTW_ESTIMATE);
        break;
    case rocfft_transform_type_complex_inverse:
        output[0].resize(osize * sizeof(fftw_complex_type));
        cpu_plan
            = fftw_plan_guru64_dft<Tfloat>(dims.size(),
                                           dims.data(),
                                           howmany_dims.size(),
                                           howmany_dims.data(),
                                           reinterpret_cast<fftw_complex_type*>(cpu_in),
                                           reinterpret_cast<fftw_complex_type*>(output[0].data()),
                                           1,
                                           FFTW_ESTIMATE);
        break;
    case rocfft_transform_type_real_forward:
        output[0].resize(osize * sizeof(fftw_complex_type));
        cpu_plan
            = fftw_plan_guru64_r2c<Tfloat>(dims.size(),
                                           dims.data(),
                                           howmany_dims.size(),
                                           howmany_dims.data(),
                                           reinterpret_cast<Tfloat*>(cpu_in),
                                           reinterpret_cast<fftw_complex_type*>(output[0].data()),
                                           FFTW_ESTIMATE);
        break;
    case rocfft_transform_type_real_inverse:
        output[0].resize(osize * sizeof(Tfloat));
        cpu_plan = fftw_plan_guru64_c2r<Tfloat>(dims.size(),
                                                dims.data(),
                                                howmany_dims.size(),
                                                howmany_dims.data(),
                                                reinterpret_cast<fftw_complex_type*>(cpu_in),
                                                reinterpret_cast<Tfloat*>(output[0].data()),
                                                FFTW_ESTIMATE);
        break;
    }

    // Execute the CPU transform:
    fftw_execute_type<Tfloat>(cpu_plan);

    fftw_destroy_plan_type(cpu_plan);

    // Cleanup CPU threads:
    fftw_clean_threads<Tfloat>();
    return output;
}

// Given input in a std::vector<char>, perform a FFT using FFTW of type given in
// transformType with the data layout given in length, istride, etc.
template <typename Tallocator>
inline std::vector<std::vector<char, Tallocator>>
    fftw_via_rocfft(const std::vector<size_t>&                  length,
                    const std::vector<size_t>&                  istride,
                    const std::vector<size_t>&                  ostride,
                    const size_t                                nbatch,
                    const size_t                                idist,
                    const size_t                                odist,
                    const rocfft_precision                      precision,
                    const rocfft_transform_type                 transformType,
                    std::vector<std::vector<char, Tallocator>>& input)
{
    const size_t dim = length.size();

    // Dimension configuration:
    std::vector<fftw_iodim64> dims(length.size());
    for(int idx = 0; idx < length.size(); ++idx)
    {
        dims[idx].n  = length[idx];
        dims[idx].is = istride[idx];
        dims[idx].os = ostride[idx];
    }

    // Batch configuration:
    std::vector<fftw_iodim64> howmany_dims(1);
    howmany_dims[0].n  = nbatch;
    howmany_dims[0].is = idist;
    howmany_dims[0].os = odist;

    switch(precision)
    {
    case rocfft_precision_single:
        return fftw_transform<float, Tallocator>(
            dims, howmany_dims, transformType, odist * nbatch, (void*)input[0].data());
        break;
    case rocfft_precision_double:
        return fftw_transform<double, Tallocator>(
            dims, howmany_dims, transformType, odist * nbatch, (void*)input[0].data());
        break;
    }
}

// Given a transform type, return the contiguous input type.
inline rocfft_array_type contiguous_itype(const rocfft_transform_type transformType)
{
    switch(transformType)
    {
    case rocfft_transform_type_complex_forward:
    case rocfft_transform_type_complex_inverse:
        return rocfft_array_type_complex_interleaved;
    case rocfft_transform_type_real_forward:
        return rocfft_array_type_real;
    case rocfft_transform_type_real_inverse:
        return rocfft_array_type_hermitian_interleaved;
    default:
        throw std::runtime_error("Invalid transform type");
    }
    return rocfft_array_type_complex_interleaved;
}

// Given a transform type, return the contiguous output type.
inline rocfft_array_type contiguous_otype(const rocfft_transform_type transformType)
{
    switch(transformType)
    {
    case rocfft_transform_type_complex_forward:
    case rocfft_transform_type_complex_inverse:
        return rocfft_array_type_complex_interleaved;
    case rocfft_transform_type_real_forward:
        return rocfft_array_type_hermitian_interleaved;
    case rocfft_transform_type_real_inverse:
        return rocfft_array_type_real;
    default:
        throw std::runtime_error("Invalid transform type");
    }
    return rocfft_array_type_complex_interleaved;
}

// Given a precision, return the acceptable tolerance.
inline double type_epsilon(const rocfft_precision precision)
{
    switch(precision)
    {
    case rocfft_precision_single:
        return type_epsilon<float>();
        break;
    case rocfft_precision_double:
        return type_epsilon<double>();
        break;
    default:
        throw std::runtime_error("Invalid precision");
        return 0.0;
    }
}

#endif
