#pragma once

#include "../buffer.h"
#include "../launch/launch_base.h"

namespace muda
{
class DeviceScan : public launch_base<DeviceScan>
{
  public:
    DeviceScan(cudaStream_t stream = nullptr)
        : launch_base(stream)
    {
    }

    template <typename T>
    DeviceScan& ExclusiveSum(device_buffer<std::byte>& external_buffer, T* d_out, T* d_in, int num_items);

    template <typename T>
    DeviceScan& InclusiveSum(device_buffer<std::byte>& external_buffer, T* d_out, T* d_in, int num_items);
};
}  // namespace muda

#ifndef __INTELLISENSE__
#include "./prefix_sum.inl"
#endif