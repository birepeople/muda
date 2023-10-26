#pragma once
#include <cuda.h>
#include <cuda_runtime.h>
#include <cuda_runtime_api.h>
#include <muda/buffer/var_view.h>

namespace muda
{
template <typename T>
class DeviceVar
{
  private:
    friend class BufferLaunch;
    T* m_data;

  public:
    using value_type = T;

    DeviceVar();
    DeviceVar(const T& value);
    DeviceVar(const DeviceVar& other);
    DeviceVar(DeviceVar&& other) MUDA_NOEXCEPT;

    // device transfer
    DeviceVar& operator=(const DeviceVar<T>& other);
    DeviceVar& operator=(VarView<T> other);
    void copy_from(VarView<T> other);

    DeviceVar& operator=(const T& val);  // copy from host
    operator T() const;  // copy to host

    T*       data() MUDA_NOEXCEPT { return m_data; }
    const T* data() const MUDA_NOEXCEPT { return m_data; }

    VarView<T> view() const MUDA_NOEXCEPT { return VarView<T>{m_data}; };
    operator VarView<T>() const MUDA_NOEXCEPT { return view(); }

    Dense<T>  viewer() MUDA_NOEXCEPT;
    CDense<T> cviewer() const MUDA_NOEXCEPT;
};

template <typename T>
Dense<T> make_dense(DeviceVar<T>& v) MUDA_NOEXCEPT;
template <typename T>
CDense<T> make_cdense(const DeviceVar<T>& v) MUDA_NOEXCEPT;
template <typename T>
Dense<T> make_viewer(DeviceVar<T>& v) MUDA_NOEXCEPT;
template <typename T>
CDense<T> make_cviewer(const DeviceVar<T>& v) MUDA_NOEXCEPT;
}  // namespace muda

#include "details/device_var.inl"