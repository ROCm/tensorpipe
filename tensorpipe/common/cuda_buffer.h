/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cuda_runtime.h>

#include <tensorpipe/common/buffer.h>

namespace tensorpipe {

struct CudaBuffer {
  void* ptr{nullptr};
  size_t length{0};
  cudaStream_t stream{cudaStreamDefault};
};

template <>
inline DeviceType deviceType(const CudaBuffer& /* unused */) {
  return DeviceType::kCuda;
}

} // namespace tensorpipe
