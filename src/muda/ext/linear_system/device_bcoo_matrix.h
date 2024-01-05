#pragma once
#include <muda/buffer/device_buffer.h>
#include <muda/ext/linear_system/bcoo_matrix_view.h>
#include <muda/ext/linear_system/device_triplet_matrix.h>

namespace muda
{
template <typename T, int N>
class DeviceBCOOMatrix : public DeviceTripletMatrix<T, N>
{
    friend class details::MatrixFormatConverter<T, N>;

  public:
    using BlockMatrix = Eigen::Matrix<T, N, N>;

    DeviceBCOOMatrix()                                         = default;
    ~DeviceBCOOMatrix()                                        = default;
    DeviceBCOOMatrix(const DeviceBCOOMatrix& other)            = default;
    DeviceBCOOMatrix(DeviceBCOOMatrix&& other)                 = default;
    DeviceBCOOMatrix& operator=(const DeviceBCOOMatrix& other) = default;
    DeviceBCOOMatrix& operator=(DeviceBCOOMatrix&& other)      = default;

    auto viewer()
    {
        return BCOOMatrixViewer<T, N>{m_block_rows,
                                      m_block_cols,
                                      0,
                                      (int)m_block_values.size(),
                                      (int)m_block_values.size(),
                                      m_block_row_indices.data(),
                                      m_block_col_indices.data(),
                                      m_block_values.data()};
    }

    auto cviewer() const
    {
        return BCOOMatrixViewer<T, N>{m_block_rows,
                                      m_block_cols,
                                      0,
                                      (int)m_block_values.size(),
                                      (int)m_block_values.size(),
                                      m_block_row_indices.data(),
                                      m_block_col_indices.data(),
                                      m_block_values.data()};
    }

    auto non_zero_blocks() const { return m_block_values.size(); }
};

template <typename Ty>
class DeviceBCOOMatrix<Ty, 1> : public DeviceTripletMatrix<Ty, 1>
{
    template <typename Ty, int N>
    friend class details::MatrixFormatConverter;

  public:
    DeviceBCOOMatrix() = default;
    ~DeviceBCOOMatrix() { destroy_all_descr(); }

    DeviceBCOOMatrix(const DeviceBCOOMatrix& other)
        : DeviceTripletMatrix{other}
        , m_legacy_descr{nullptr}
        , m_descr{nullptr}
    {
    }

    DeviceBCOOMatrix(DeviceBCOOMatrix&& other)
        : DeviceTripletMatrix{std::move(other)}
        , m_legacy_descr{other.m_legacy_descr}
        , m_descr{other.m_descr}
    {
        other.m_legacy_descr = nullptr;
        other.m_descr        = nullptr;
    }

    DeviceBCOOMatrix& operator=(const DeviceBCOOMatrix& other)
    {
        if(this == &other)
            return *this;
        DeviceTripletMatrix::operator=(other);
        destroy_all_descr();
        m_legacy_descr = nullptr;
        m_descr        = nullptr;
        return *this;
    }

    DeviceBCOOMatrix& operator=(DeviceBCOOMatrix&& other)
    {
        if(this == &other)
            return *this;
        DeviceTripletMatrix::operator=(std::move(other));
        destroy_all_descr();
        m_legacy_descr       = other.m_legacy_descr;
        m_descr              = other.m_descr;
        other.m_legacy_descr = nullptr;
        other.m_descr        = nullptr;
        return *this;
    }

    mutable cusparseMatDescr_t   m_legacy_descr = nullptr;
    mutable cusparseSpMatDescr_t m_descr        = nullptr;

    auto view()
    {
        return COOMatrixView<Ty>{m_rows,
                                 m_cols,
                                 0,
                                 (int)m_values.size(),
                                 (int)m_values.size(),
                                 m_row_indices.data(),
                                 m_col_indices.data(),
                                 m_values.data(),
                                 legacy_descr(),
                                 descr(),
                                 false};
    }

    auto view() const
    {
        return CCOOMatrixView<Ty>{m_rows,
                                  m_cols,
                                  0,
                                  (int)m_values.size(),
                                  (int)m_values.size(),
                                  m_row_indices.data(),
                                  m_col_indices.data(),
                                  m_values.data(),
                                  legacy_descr(),
                                  descr(),
                                  false};
    }

    auto viewer() { return view().viewer(); }

    auto cviewer() const { return view().cviewer(); }

    auto non_zeros() const { return m_values.size(); }

    auto legacy_descr() const
    {
        if(m_legacy_descr == nullptr)
        {
            checkCudaErrors(cusparseCreateMatDescr(&m_legacy_descr));
            checkCudaErrors(cusparseSetMatType(m_legacy_descr, CUSPARSE_MATRIX_TYPE_GENERAL));
            checkCudaErrors(cusparseSetMatIndexBase(m_legacy_descr, CUSPARSE_INDEX_BASE_ZERO));
        }
        return m_legacy_descr;
    }

    auto descr() const
    {
        if(m_descr == nullptr)
        {
            checkCudaErrors(cusparseCreateCoo(&m_descr,
                                              m_rows,
                                              m_cols,
                                              non_zeros(),
                                              (void*)m_row_indices.data(),
                                              (void*)m_col_indices.data(),
                                              (void*)m_values.data(),
                                              CUSPARSE_INDEX_32I,
                                              CUSPARSE_INDEX_BASE_ZERO,
                                              cuda_data_type<Ty>()));
        }
        return m_descr;
    }

    auto T() const { return view().T(); }
    auto T() { return view().T(); }

    operator COOMatrixView<Ty>() { return view(); }
    operator CCOOMatrixView<Ty>() const { return view(); }

  private:
    void destroy_all_descr()
    {
        if(m_legacy_descr != nullptr)
        {
            checkCudaErrors(cusparseDestroyMatDescr(m_legacy_descr));
            m_legacy_descr = nullptr;
        }
        if(m_descr != nullptr)
        {
            checkCudaErrors(cusparseDestroySpMat(m_descr));
            m_descr = nullptr;
        }
    }
};

template <typename T>
using DeviceCOOMatrix = DeviceBCOOMatrix<T, 1>;
}  // namespace muda

#include "details/device_bcoo_matrix.inl"