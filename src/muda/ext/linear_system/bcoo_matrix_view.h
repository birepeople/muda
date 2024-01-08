#pragma once
#include <muda/ext/linear_system/triplet_matrix_view.h>
#include <muda/ext/linear_system/bcoo_matrix_viewer.h>
namespace muda
{
template <typename T, int N>
using BCOOMatrixView = TripletMatrixView<T, N>;
template <typename T, int N>
using CBCOOMatrixView = CTripletMatrixView<T, N>;
}  // namespace muda
namespace muda
{
template <bool IsConst, typename Ty>
class COOMatrixViewBase : public ViewBase<IsConst>
{
  public:
    static_assert(!std::is_const_v<Ty>, "Ty must be non-const");
    using NonConstView = COOMatrixViewBase<false, Ty>;
    using ConstView    = COOMatrixViewBase<true, Ty>;
    using ThisView     = COOMatrixViewBase<IsConst, Ty>;

  protected:
    // matrix info
    int m_rows = 0;
    int m_cols = 0;

    // triplet info
    int m_triplet_index_offset = 0;
    int m_triplet_count        = 0;
    int m_total_triplet_count  = 0;

    // data
    auto_const_t<int>* m_row_indices;
    auto_const_t<int>* m_col_indices;
    auto_const_t<Ty>*  m_values;

    mutable cusparseMatDescr_t   m_legacy_descr = nullptr;
    mutable cusparseSpMatDescr_t m_descr        = nullptr;
    bool                         m_trans        = false;

  public:
    MUDA_GENERIC COOMatrixViewBase() = default;
    MUDA_GENERIC COOMatrixViewBase(int                  rows,
                                   int                  cols,
                                   int                  triplet_index_offset,
                                   int                  triplet_count,
                                   int                  total_triplet_count,
                                   auto_const_t<int>*   row_indices,
                                   auto_const_t<int>*   col_indices,
                                   auto_const_t<Ty>*    values,
                                   cusparseSpMatDescr_t descr,
                                   cusparseMatDescr_t   legacy_descr,
                                   bool                 trans)

        : m_rows(rows)
        , m_cols(cols)
        , m_triplet_index_offset(triplet_index_offset)
        , m_triplet_count(triplet_count)
        , m_total_triplet_count(total_triplet_count)
        , m_row_indices(row_indices)
        , m_col_indices(col_indices)
        , m_values(values)
        , m_descr(descr)
        , m_legacy_descr(legacy_descr)
        , m_trans(trans)
    {
        MUDA_KERNEL_ASSERT(triplet_index_offset + triplet_count <= total_triplet_count,
                           "COOMatrixView: out of range, m_total_triplet_count=%d, "
                           "your triplet_index_offset=%d, triplet_count=%d",
                           total_triplet_count,
                           triplet_index_offset,
                           triplet_count);
    }


    MUDA_GENERIC auto as_const() const
    {
        return ConstView{m_rows,
                         m_cols,
                         m_triplet_index_offset,
                         m_triplet_count,
                         m_total_triplet_count,
                         m_row_indices,
                         m_col_indices,
                         m_values,
                         m_descr,
                         m_legacy_descr,
                         m_trans};
    }

    MUDA_GENERIC operator ConstView() const { return as_const(); }

    MUDA_GENERIC auto cviewer() const
    {
        MUDA_KERNEL_ASSERT(!m_trans,
                           "COOMatrixView: cviewer() is not supported for "
                           "transposed matrix, please use a non-transposed view of this matrix");
        return CTripletMatrixViewer<Ty, 1>{m_rows,
                                           m_cols,
                                           m_triplet_index_offset,
                                           m_triplet_count,
                                           m_total_triplet_count,
                                           m_row_indices,
                                           m_col_indices,
                                           m_values};
    }

    MUDA_GENERIC auto viewer()
    {
        MUDA_ASSERT(!m_trans,
                    "COOMatrixView: viewer() is not supported for "
                    "transposed matrix, please use a non-transposed view of this matrix");
        return TripletMatrixViewer<Ty, 1>{m_rows,
                                          m_cols,
                                          m_triplet_index_offset,
                                          m_triplet_count,
                                          m_total_triplet_count,
                                          m_row_indices,
                                          m_col_indices,
                                          m_values};
    }

    MUDA_GENERIC auto subview(int offset, int count)
    {
        return ThisView{m_rows,
                        m_cols,
                        m_triplet_index_offset + offset,
                        count,
                        m_total_triplet_count,
                        m_row_indices,
                        m_col_indices,
                        m_values,
                        m_descr,
                        m_legacy_descr,
                        m_trans};
    }

    MUDA_GENERIC auto subview(int offset)
    {
        MUDA_ASSERT(offset < m_triplet_count,
                    "TripletMatrixView: offset is out of range, size=%d, your offset=%d",
                    m_triplet_count,
                    offset);
        return subview(offset, m_triplet_count - offset);
    }

    MUDA_GENERIC ConstView subview(int offset, int count) const
    {
        return remove_const(*this).subview(offset, count);
    }

    MUDA_GENERIC ConstView subview(int offset) const
    {
        return remove_const(*this).subview(offset);
    }

    // non-const access
    auto_const_t<Ty>*  block_values() { return m_values; }
    auto_const_t<int>* block_row_indices() { return m_row_indices; }
    auto_const_t<int>* block_col_indices() { return m_col_indices; }


    // const access
    auto block_values() const { return m_values; }
    auto block_row_indices() const { return m_row_indices; }
    auto block_col_indices() const { return m_col_indices; }

    auto block_rows() const { return m_rows; }
    auto block_cols() const { return m_cols; }
    auto triplet_count() const { return m_triplet_count; }
    auto tripet_index_offset() const { return m_triplet_index_offset; }
    auto total_triplet_count() const { return m_total_triplet_count; }
    auto is_trans() const { return m_trans; }
};

template <typename Ty>
using COOMatrixView = COOMatrixViewBase<false, Ty>;
template <typename Ty>
using CCOOMatrixView = COOMatrixViewBase<true, Ty>;
}  // namespace muda

namespace muda
{
template <typename T>
struct read_only_viewer<COOMatrixView<T>>
{
    using type = CCOOMatrixView<T>;
};

template <typename T>
struct read_write_viewer<CCOOMatrixView<T>>
{
    using type = COOMatrixView<T>;
};
}  // namespace muda
#include "details/bcoo_matrix_view.inl"
