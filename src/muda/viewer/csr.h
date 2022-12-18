#pragma once
#include "mapper.h"

namespace muda
{
/// <summary>
/// a viwer that allows to access a CSR sparse matrix
/// </summary>
/// <typeparam name="T"></typeparam>
template <typename T>
class csr
{
  public:
    class elem
    {
        int     row_;
        int     col_;
        int     global_offset_;
        csr<T>& csr_;
        MUDA_GENERIC elem(csr<T>& csr, int row, int col, int global_offset) noexcept
            : csr_(csr)
            , row_(row)
            , col_(col)
            , global_offset_(global_offset)
        {
        }

      public:
        friend class csr<T>;
        //trivial copy constructor
        MUDA_GENERIC elem(const elem& e) = default;
        //trivial copy assignment
        MUDA_GENERIC elem& operator=(const elem& e) = default;
        MUDA_GENERIC       operator const T&() const noexcept
        {
            return csr_.values_[global_offset_];
        }
        MUDA_GENERIC operator T&() noexcept
        {
            return csr_.values_[global_offset_];
        }
        Eigen::Vector<int, 2> pos() const noexcept
        {
            return Eigen::Vector<int, 2>(row_, col_);
        }
        int global_offset() const noexcept { return global_offset_; }

        MUDA_GENERIC T& operator=(const T& v) noexcept
        {
            auto& pos = csr_.values_[global_offset_];
            pos       = v;
            return pos;
        }
    };
    class celem
    {
        int           row_;
        int           col_;
        int           global_offset_;
        const csr<T>& csr_;
        MUDA_GENERIC celem(const csr<T>& csr, int row, int col, int global_offset) noexcept
            : csr_(csr)
            , row_(row)
            , col_(col)
            , global_offset_(global_offset)
        {
        }

      public:
        friend class csr<T>;
        //trivial copy constructor
        MUDA_GENERIC celem(const celem& e) = default;
        //trivial copy assignment
        MUDA_GENERIC celem& operator=(const celem& e) = default;
        MUDA_GENERIC        operator const T&() const noexcept
        {
            return csr_.values_[global_offset_];
        }
        Eigen::Vector<int, 2> pos() const noexcept
        {
            return Eigen::Vector<int, 2>(row_, col_);
        }
        int global_offset() const noexcept { return global_offset_; }
    };

    MUDA_GENERIC csr() noexcept
        : values_(nullptr)
        , colIdx_(nullptr)
        , rowPtr_(nullptr)
        , nnz_(0)
        , rows_(0)
        , cols_(0)
    {
    }
    MUDA_GENERIC csr(int* rowPtr, int* colIdx, T* values, int rows, int cols, int nNonZeros) noexcept
        : rowPtr_(rowPtr)
        , colIdx_(colIdx)
        , values_(values)
        , nnz_(nNonZeros)
        , rows_(rows)
        , cols_(cols)
    {
    }
    // rows getter
    MUDA_GENERIC int rows() const noexcept { return rows_; }
    // cols getter
    MUDA_GENERIC int cols() const noexcept { return cols_; }
    // nnz getter
    MUDA_GENERIC int nnz() const noexcept { return nnz_; }

    // get by row and col as if it is a dense matrix
    MUDA_GENERIC T operator()(int row, int col) const noexcept
    {
        checkRange(row, col);
        for(int i = rowPtr_[row]; i < rowPtr_[row + 1]; i++)
        {
            if(colIdx_[i] == col)
                return values_[i];
        }
        return 0;
    }
    // read-write element
    MUDA_GENERIC elem rw_elem(int row, int local_offset) noexcept
    {
        int global_offset;
        checkAll(row, local_offset, global_offset);
        return elem(*this, row, colIdx_[global_offset], global_offset);
    }
    // read-only element
    MUDA_GENERIC celem ro_elem(int row, int local_offset) const noexcept
    {
        int global_offset;
        checkAll(row, local_offset, global_offset);
        return celem(*this, row, colIdx_[global_offset], global_offset);
    }

    MUDA_GENERIC void place_row(int row, int global_offset) noexcept
    {
        checkRow(row);
        rowPtr_[row] = global_offset;
    }

    MUDA_GENERIC void place_tail() noexcept { rowPtr_[rows_] = nnz_; }

    MUDA_GENERIC int place_col(int row, int local_offset, int col) noexcept
    {
        checkRow(row);
        int global_offset = rowPtr_[row] + local_offset;
        checkGlobalOffset(global_offset);
        colIdx_[global_offset] = col;
        return global_offset;
    }

    MUDA_GENERIC int place_col(int row, int local_offset, int col, const T& v) noexcept
    {
        checkRow(row);
        int global_offset = rowPtr_[row] + local_offset;
        checkGlobalOffset(global_offset);
        colIdx_[global_offset] = col;
        values_[global_offset] = v;
        return global_offset;
    }

    MUDA_GENERIC int nnz(int row) const noexcept
    {
        checkRow(row);
        return rowPtr_[row + 1] - rowPtr_[row];
    }

  private:
    int* rowPtr_;
    int* colIdx_;
    T*   values_;
    int  nnz_;
    int  rows_;
    int  cols_;
    MUDA_GENERIC __forceinline__ void checkRange(int row, int col) const noexcept
    {
        if constexpr(debugViewers)
            if(row < 0 || row >= rows_ || col < 0 || col >= cols_)
            {
                muda_kernel_printf("row/col index out of range: index=(%d,%d) dim_=(%d,%d)\n",
                                   row,
                                   col,
                                   rows_,
                                   cols_);
                if constexpr(trapOnError)
                    trap();
            }
    }

    MUDA_GENERIC __forceinline__ void checkRow(int row) const noexcept
    {
        if constexpr(debugViewers)
            if(row < 0 || row >= rows_)
            {
                muda_kernel_printf("row index out of range: index=(%d) rows=(%d)\n", row, rows_);
                if constexpr(trapOnError)
                    trap();
            }
    }

    MUDA_GENERIC __forceinline__ void checkLocalOffset(int row, int offset) const noexcept
    {
        if constexpr(debugViewers)
            if(row < 0 || row >= rows_ || offset < 0
               || offset >= rowPtr_[row + 1] - rowPtr_[row])
            {
                muda_kernel_printf(
                    "'rowPtr[row] + offset > rowPtr[row+1]' out of range:\n"
                    "row=%d, offset=%d, rowPtr[row]=%d, rowPtr[row+1]=%d\n",
                    row,
                    offset,
                    rowPtr_[row],
                    rowPtr_[row + 1]);
                if constexpr(trapOnError)
                    trap();
            }
    }

    MUDA_GENERIC __forceinline__ void checkGlobalOffset(int globalOffset) const noexcept
    {
        if constexpr(debugViewers)
            if(globalOffset < 0 || globalOffset >= nnz_)
            {
                muda_kernel_printf("globalOffset out of range: globalOffset=%d, nnz=%d\n",
                                   globalOffset,
                                   nnz_);
                if constexpr(trapOnError)
                    trap();
            }
    }

    MUDA_GENERIC __forceinline__ void checkAll(int row, int local_offset, int& global_offset) const noexcept
    {
        checkRow(row);
        checkLocalOffset(row, local_offset);
        global_offset = rowPtr_[row] + local_offset;
        checkGlobalOffset(global_offset);
    }
};
}  // namespace muda
