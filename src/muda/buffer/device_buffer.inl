#ifdef __INTELLISENSE__
#include "device_buffer.h"
#endif

namespace muda
{
template <typename T>
Empty DeviceBuffer<T>::resize(size_t new_size, BufferOperation mem_op, char setbyte)
{
    auto   mem      = Memory(m_stream);
    size_t old_size = m_size;

    if(new_size <= m_size)
    {
        switch(mem_op)
        {
            case muda::BufferOperation::set:
                mem.set(m_data, new_size * sizeof(value_type), (int)setbyte);
                break;
            default:
                break;
        }
        m_size = new_size;
        return Empty(m_stream);
    }

    if(new_size <= m_capacity)
    {
        switch(mem_op)
        {
            case muda::BufferOperation::set:
                mem.set(m_data, new_size * sizeof(value_type), (int)setbyte);
                break;
            case muda::BufferOperation::keep_set:
                mem.set(m_data + old_size,
                        (new_size - old_size) * sizeof(value_type),
                        (int)setbyte);
                break;
            default:
                break;
        }
        m_size = new_size;
    }
    else
    {
        T* ptr;
        mem.alloc(&ptr, new_size * sizeof(value_type));
        switch(mem_op)
        {
            case muda::BufferOperation::keep:
                mem.copy(ptr, m_data, old_size * sizeof(value_type), cudaMemcpyDeviceToDevice);
                break;
            case muda::BufferOperation::set:
                mem.set(ptr, new_size * sizeof(value_type), (int)setbyte);
                break;
            case muda::BufferOperation::keep_set:
                if(m_data)
                    mem.copy(ptr, m_data, old_size * sizeof(value_type), cudaMemcpyDeviceToDevice);
                mem.set(ptr + old_size, (new_size - old_size) * sizeof(value_type), (int)setbyte);
                break;
            default:
                break;
        }
        if(m_data)
            mem.free(m_data);
        m_data     = ptr;
        m_size     = new_size;
        m_capacity = new_size;
    }

    return Empty(m_stream);
}

template <typename T>
Empty DeviceBuffer<T>::resize(size_t new_size)
{
    auto   mem      = Memory(m_stream);
    size_t old_size = m_size;

    if(new_size <= m_size)
    {
        m_size = new_size;
        return Empty(m_stream);
    }

    if(new_size <= m_capacity)
    {
        mem.set(m_data + old_size, (new_size - old_size) * sizeof(value_type), 0);
        m_size = new_size;
    }
    else
    {
        T* ptr;
        mem.alloc(&ptr, new_size * sizeof(value_type));
        if(m_data)
            mem.copy(ptr, m_data, old_size * sizeof(value_type), cudaMemcpyDeviceToDevice);
        mem.set(ptr + old_size, (new_size - old_size) * sizeof(value_type), 0);
        if(m_data)
            mem.free(m_data);
        m_data     = ptr;
        m_size     = new_size;
        m_capacity = new_size;
    }

    return Empty(m_stream);
}

template <typename T>
inline Empty DeviceBuffer<T>::resize(size_t new_size, const value_type& value, int blockDim)
{
    auto   mem      = Memory(m_stream);
    size_t old_size = m_size;

    if(new_size <= m_capacity)
    {
        m_size = new_size;
    }
    else
    {
        T* ptr;
        mem.alloc(&ptr, new_size * sizeof(value_type));
        if(m_data)
            mem.free(m_data);
        m_data     = ptr;
        m_size     = new_size;
        m_capacity = new_size;
    }

    ParallelFor(blockDim, 0, m_stream)
        .apply(new_size,
               [=, d = make_viewer(*this)] __device__(int i) mutable
               { d(i) = value; });

    return Empty(m_stream);
}

template <typename T>
Empty DeviceBuffer<T>::shrink_to_fit()
{
    auto mem = Memory(m_stream);

    if(m_size < m_capacity)
    {
        T* ptr;
        mem.alloc(&ptr, m_size * sizeof(value_type));
        mem.copy(ptr, m_data, m_size * sizeof(value_type), cudaMemcpyDeviceToDevice);
        if(m_data)
            mem.free(m_data);
        m_data     = ptr;
        m_capacity = m_size;
    }

    return Empty(m_stream);
}

template <typename T>
Empty DeviceBuffer<T>::set(char setbyte, size_t count)
{
    m_init = true;
    if(count == size_t(-1))
        count = m_size;
    if(count > m_size)
        throw std::out_of_range("device_buffer::set out of range");
    Memory(m_stream).set(m_data, count * sizeof(T), setbyte);
    return Empty(m_stream);
}

template <typename T>
Empty DeviceBuffer<T>::fill(const T& value, size_t count, int blockDim)
{
    m_init = true;
    if(count == size_t(-1))
        count = m_size;
    if(count > m_size)
        throw std::out_of_range("device_buffer::set out of range");
    auto   mem      = Memory(m_stream);
    ParallelFor(blockDim, 0, m_stream)
        .apply(m_size,
               [=, d = make_viewer(*this)] __device__(int i) mutable
               { d(i) = value; });
    return Empty(m_stream);
}
template <typename T>
Empty DeviceBuffer<T>::copy_to(value_type& var) const
{
    if(m_size != 1)
        throw std::logic_error("buffer size larger than 1, cannot copy to host_var");
    m_init = true;
    Memory(m_stream).copy(std::addressof(var), m_data, m_size * sizeof(value_type), cudaMemcpyDeviceToHost);
    return Empty(m_stream);
}
template <typename T>
Empty DeviceBuffer<T>::copy_to(HostVector<value_type>& vec) const
{
    m_init = true;
    vec.resize(m_size);
    Memory(m_stream).copy(muda::data(vec), m_data, m_size * sizeof(value_type), cudaMemcpyDeviceToHost);
    return Empty(m_stream);
}
template <typename T>
Empty DeviceBuffer<T>::copy_to(DeviceVar<value_type>& var) const
{
    if(m_size != 1)
        throw std::logic_error("buffer size larger than 1, cannot copy to device_var");
    m_init = true;
    Memory(m_stream).copy(muda::data(var), m_data, m_size * sizeof(value_type), cudaMemcpyDeviceToDevice);
    return Empty(m_stream);
}
template <typename T>
Empty DeviceBuffer<T>::copy_to(DeviceVector<value_type>& vec) const
{
    m_init = true;
    vec.resize(m_size);
    Memory(m_stream).copy(muda::data(vec), m_data, m_size * sizeof(value_type), cudaMemcpyDeviceToDevice);
    return Empty(m_stream);
}
template <typename T>
Empty DeviceBuffer<T>::copy_to(DeviceBuffer<value_type>& vec) const
{
    m_init = true;
    vec.resize(m_size);
    Memory(m_stream).copy(vec.data(), m_data, m_size * sizeof(value_type), cudaMemcpyDeviceToDevice);
    return Empty(m_stream);
}
template <typename T>
Empty DeviceBuffer<T>::copy_from(const host_var<value_type>& var)
{
    m_init = true;
    resize(1);
    Memory(m_stream).copy(m_data, muda::data(var), m_size * sizeof(value_type), cudaMemcpyHostToDevice);
    return Empty(m_stream);
}
template <typename T>
Empty DeviceBuffer<T>::copy_from(const value_type& var)
{
    m_init = true;
    resize(1);
    Memory(m_stream).copy(m_data, std::addressof(var), m_size * sizeof(value_type), cudaMemcpyHostToDevice);
    return Empty(m_stream);
}
template <typename T>
Empty DeviceBuffer<T>::copy_from(const HostVector<value_type>& vec)
{
    m_init = true;
    resize(vec.size());
    Memory(m_stream).copy(m_data, muda::data(vec), m_size * sizeof(value_type), cudaMemcpyHostToDevice);
    return Empty(m_stream);
}
template <typename T>
Empty DeviceBuffer<T>::copy_from(const DeviceVar<value_type>& var)
{
    m_init = true;
    resize(1);
    Memory(m_stream).copy(m_data, muda::data(var), m_size * sizeof(value_type), cudaMemcpyDeviceToDevice);
    return Empty(m_stream);
}
template <typename T>
Empty DeviceBuffer<T>::copy_from(const DeviceVector<value_type>& vec)
{
    m_init = true;
    resize(vec.size());
    Memory().copy(m_data, muda::data(vec), m_size * sizeof(value_type), cudaMemcpyDeviceToDevice);
    return Empty(m_stream);
}
template <typename T>
Empty DeviceBuffer<T>::copy_from(const DeviceBuffer<value_type>& vec)
{
    m_init = true;
    resize(vec.size());
    Memory().copy(m_data, vec.data(), m_size * sizeof(value_type), cudaMemcpyDeviceToDevice);
    return Empty(m_stream);
}
}