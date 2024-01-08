#include <muda/cub/device/device_merge_sort.h>
#include <muda/cub/device/device_run_length_encode.h>
#include <muda/cub/device/device_scan.h>
#include <muda/cub/device/device_segmented_reduce.h>
#include <muda/launch.h>

// for encode run length usage
MUDA_GENERIC constexpr bool operator==(const int2& a, const int2& b)
{
    return a.x == b.x && a.y == b.y;
}

namespace muda::details
{
template <typename T, int N>
void MatrixFormatConverter<T, N>::convert(const DeviceTripletMatrix<T, N>& from,
                                          DeviceBCOOMatrix<T, N>&          to)
{
    to.reshape(from.block_rows(), from.block_cols());
    to.m_block_row_indices.resize(from.m_block_row_indices.size());
    to.m_block_col_indices.resize(from.m_block_col_indices.size());
    merge_sort_indices_and_blocks(from, to);
    make_unique_indices(from, to);
    make_unique_blocks(from, to);
}

template <typename T, int N>
void MatrixFormatConverter<T, N>::merge_sort_indices_and_blocks(
    const DeviceTripletMatrix<T, N>& from, DeviceBCOOMatrix<T, N>& to)
{
    using namespace muda;

    auto src_row_indices = from.block_row_indices();
    auto src_col_indices = from.block_col_indices();
    auto src_blocks      = from.block_values();

    sort_index.resize(src_row_indices.size());
    ij_pairs.resize(src_row_indices.size());

    ParallelFor(256)
        .kernel_name("set ij pairs")
        .apply(src_row_indices.size(),
               [row_indices = src_row_indices.cviewer().name("row_indices"),
                col_indices = src_col_indices.cviewer().name("col_indices"),
                ij_pairs = ij_pairs.viewer().name("ij_pairs")] __device__(int i) mutable
               {
                   ij_pairs(i).x = row_indices(i);
                   ij_pairs(i).y = col_indices(i);
               });

    ParallelFor(256)
        .kernel_name("iota")  //
        .apply(src_row_indices.size(),
               [sort_index = sort_index.viewer().name("sort_index")] __device__(int i) mutable
               { sort_index(i) = i; });

    DeviceMergeSort().SortPairs(workspace,
                                ij_pairs.data(),
                                sort_index.data(),
                                ij_pairs.size(),
                                [] __device__(const int2& a, const int2& b) {
                                    return a.x < b.x || (a.x == b.x && a.y < b.y);
                                });

    // set ij_pairs back to row_indices and col_indices

    auto dst_row_indices = to.block_row_indices();
    auto dst_col_indices = to.block_col_indices();

    ParallelFor(256)
        .kernel_name("set col row indices")
        .apply(dst_row_indices.size(),
               [row_indices = dst_row_indices.viewer().name("row_indices"),
                col_indices = dst_col_indices.viewer().name("col_indices"),
                ij_pairs = ij_pairs.viewer().name("ij_pairs")] __device__(int i) mutable
               {
                   row_indices(i) = ij_pairs(i).x;
                   col_indices(i) = ij_pairs(i).y;
               });

    // sort the block values

    unique_blocks.resize(from.m_block_values.size());

    ParallelFor(256)
        .kernel_name("set block values")
        .apply(src_blocks.size(),
               [src_blocks = src_blocks.cviewer().name("blocks"),
                sort_index = sort_index.cviewer().name("sort_index"),
                dst_blocks = unique_blocks.viewer().name("block_values")] __device__(int i) mutable
               { dst_blocks(i) = src_blocks(sort_index(i)); });
}

template <typename T, int N>
void MatrixFormatConverter<T, N>::make_unique_indices(const DeviceTripletMatrix<T, N>& from,
                                                      DeviceBCOOMatrix<T, N>& to)
{
    using namespace muda;

    auto& row_indices = to.m_block_row_indices;
    auto& col_indices = to.m_block_col_indices;

    unique_ij_pairs.resize(ij_pairs.size());
    unique_counts.resize(ij_pairs.size());

    DeviceRunLengthEncode().Encode(workspace,
                                   ij_pairs.data(),
                                   unique_ij_pairs.data(),
                                   unique_counts.data(),
                                   count.data(),
                                   ij_pairs.size());

    int h_count = count;

    unique_ij_pairs.resize(h_count);
    unique_counts.resize(h_count);

    offsets.resize(unique_counts.size());

    DeviceScan().ExclusiveSum(
        workspace, unique_counts.data(), offsets.data(), unique_counts.size());


    muda::ParallelFor(256)
        .kernel_name("make unique indices")
        .apply(unique_counts.size(),
               [unique_ij_pairs = unique_ij_pairs.viewer().name("unique_ij_pairs"),
                row_indices = row_indices.viewer().name("row_indices"),
                col_indices = col_indices.viewer().name("col_indices")] __device__(int i) mutable
               {
                   row_indices(i) = unique_ij_pairs(i).x;
                   col_indices(i) = unique_ij_pairs(i).y;
               });

    row_indices.resize(h_count);
    col_indices.resize(h_count);
}

template <typename T, int N>
void MatrixFormatConverter<T, N>::make_unique_blocks(const DeviceTripletMatrix<T, N>& from,
                                                     DeviceBCOOMatrix<T, N>& to)
{
    using namespace muda;

    auto& row_indices = to.m_block_row_indices;
    auto& blocks      = to.m_block_values;
    blocks.resize(row_indices.size());
    // first we add the offsets to counts, to get the offset_ends

    ParallelFor(256)
        .kernel_name("calculate offset_ends")
        .apply(unique_counts.size(),
               [offset = offsets.cviewer().name("offset"),
                counts = unique_counts.viewer().name("counts")] __device__(int i) mutable
               { counts(i) += offset(i); });

    auto& begin_offset = offsets;
    auto& end_offset   = unique_counts;  // already contains the offset_ends

    // then we do a segmented reduce to get the unique blocks
    DeviceSegmentedReduce().Reduce(
        workspace,
        unique_blocks.data(),
        blocks.data(),
        blocks.size(),
        offsets.data(),
        end_offset.data(),
        [] __host__ __device__(const BlockMatrix& a, const BlockMatrix& b) -> BlockMatrix
        { return a + b; },
        BlockMatrix::Zero().eval());
}

template <typename T, int N>
void MatrixFormatConverter<T, N>::convert(const DeviceBCOOMatrix<T, N>& from,
                                          DeviceDenseMatrix<T>&         to,
                                          bool clear_dense_matrix)
{
    using namespace muda;
    auto size = N * from.block_rows();
    to.reshape(size, size);

    if(clear_dense_matrix)
        to.fill(0);

    auto& cast = const_cast<DeviceBCOOMatrix<T, N>&>(from);

    ParallelFor(256)
        .kernel_name(__FUNCTION__)
        .apply(from.block_values().size(),
               [blocks = cast.viewer().name("src_sparse_matrix"),
                dst = to.viewer().name("dst_dense_matrix")] __device__(int i) mutable
               {
                   auto block                = blocks(i);
                   auto row                  = block.block_row_index * N;
                   auto col                  = block.block_col_index * N;
                   dst.block<N, N>(row, col) = block.block;
               });
}

template <typename T, int N>
void MatrixFormatConverter<T, N>::convert(const DeviceBCOOMatrix<T, N>& from,
                                          DeviceBSRMatrix<T, N>&        to)
{
    calculate_block_offsets(from, to);
    to.m_block_col_indices = from.m_block_col_indices;
    to.m_block_values      = from.m_block_values;
}

template <typename T, int N>
void MatrixFormatConverter<T, N>::convert(DeviceBCOOMatrix<T, N>&& from,
                                          DeviceBSRMatrix<T, N>&   to)
{
    calculate_block_offsets(from, to);
    to.m_block_col_indices = std::move(from.m_block_col_indices);
    to.m_block_values      = std::move(from.m_block_values);
}

template <typename T, int N>
void MatrixFormatConverter<T, N>::calculate_block_offsets(const DeviceBCOOMatrix<T, N>& from,
                                                          DeviceBSRMatrix<T, N>& to)
{
    using namespace muda;
    to.reshape(from.block_rows(), from.block_cols());

    auto& dst_row_offsets = to.m_block_row_offsets;

    // alias the offsets to the col_counts_per_row(reuse)
    auto& col_counts_per_row = offsets;
    col_counts_per_row.resize(to.m_block_row_offsets.size());
    col_counts_per_row.fill(0);

    unique_indices.resize(from.non_zero_blocks());
    unique_counts.resize(from.non_zero_blocks());

    // run length encode the row
    DeviceRunLengthEncode().Encode(workspace,
                                   from.m_block_row_indices.data(),
                                   unique_indices.data(),
                                   unique_counts.data(),
                                   count.data(),
                                   from.non_zero_blocks());
    int h_count = count;

    unique_indices.resize(h_count);
    unique_counts.resize(h_count);

    ParallelFor(256)
        .kernel_name(__FUNCTION__)
        .apply(unique_counts.size(),
               [unique_indices     = unique_indices.cviewer().name("offset"),
                counts             = unique_counts.viewer().name("counts"),
                col_counts_per_row = col_counts_per_row.viewer().name(
                    "col_counts_per_row")] __device__(int i) mutable
               {
                   auto row                = unique_indices(i);
                   col_counts_per_row(row) = counts(i);
               });

    // calculate the offsets
    DeviceScan().ExclusiveSum(workspace,
                              col_counts_per_row.data(),
                              dst_row_offsets.data(),
                              col_counts_per_row.size());
}
template <typename T, int N>
void MatrixFormatConverter<T, N>::convert(const DeviceDoubletVector<T, N>& from,
                                          DeviceDenseVector<T>&            to,
                                          bool clear_dense_vector)
{
    to.resize(N * from.segment_count());
    merge_sort_indices_and_segments(from, to);
    make_unique_indices(from, to);
    make_unique_segments(from, to);
    set_unique_segments_to_dense_vector(from, to, clear_dense_vector);
}

template <typename T, int N>
void MatrixFormatConverter<T, N>::merge_sort_indices_and_segments(
    const DeviceDoubletVector<T, N>& from, DeviceDenseVector<T>& to)
{
    using namespace muda;

    auto& index = sort_index;  // alias sort_index to index
    // copy as temp
    index         = from.m_segment_indices;
    temp_segments = from.m_segment_values;

    DeviceMergeSort().SortPairs(workspace,
                                index.data(),
                                temp_segments.data(),
                                index.size(),
                                [] __device__(const int& a, const int& b)
                                { return a < b; });
}

template <typename T, int N>
void MatrixFormatConverter<T, N>::make_unique_indices(const DeviceDoubletVector<T, N>& from,
                                                      DeviceDenseVector<T>& to)
{
    using namespace muda;

    auto& index = sort_index;  // alias sort_index to index

    unique_indices.resize(index.size());
    unique_counts.resize(index.size());

    DeviceRunLengthEncode().Encode(workspace,
                                   index.data(),
                                   unique_indices.data(),
                                   unique_counts.data(),
                                   count.data(),
                                   index.size());

    int h_count = count;

    unique_indices.resize(h_count);
    unique_counts.resize(h_count);

    offsets.resize(unique_counts.size());

    DeviceScan().ExclusiveSum(
        workspace, unique_counts.data(), offsets.data(), unique_counts.size());

    // calculate the offset_ends, and set to the unique_counts

    auto& begin_offset = offsets;
    auto& end_offset   = unique_counts;

    ParallelFor(256)
        .kernel_name("calculate offset_ends")
        .apply(unique_counts.size(),
               [offset = offsets.cviewer().name("offset"),
                counts = unique_counts.viewer().name("counts")] __device__(int i) mutable
               { counts(i) += offset(i); });
}

template <typename T, int N>
void MatrixFormatConverter<T, N>::make_unique_segments(const DeviceDoubletVector<T, N>& from,
                                                       DeviceDenseVector<T>& to)
{
    using namespace muda;

    auto& begin_offset = offsets;
    auto& end_offset   = unique_counts;

    unique_segments.resize(unique_indices.size());

    DeviceSegmentedReduce().Reduce(
        workspace,
        temp_segments.data(),
        unique_segments.data(),
        unique_segments.size(),
        begin_offset.data(),
        end_offset.data(),
        [] __host__ __device__(const SegmentVector& a, const SegmentVector& b) -> SegmentVector
        { return a + b; },
        SegmentVector::Zero().eval());
}
template <typename T, int N>
void MatrixFormatConverter<T, N>::set_unique_segments_to_dense_vector(
    const DeviceDoubletVector<T, N>& from, DeviceDenseVector<T>& to, bool clear_dense_vector)
{
    using namespace muda;

    if(clear_dense_vector)
        to.fill(0);

    ParallelFor(256)
        .kernel_name("set unique segments to dense vector")
        .apply(unique_segments.size(),
               [unique_segments = unique_segments.viewer().name("unique_segments"),
                unique_indices = unique_indices.viewer().name("unique_indices"),
                dst = to.viewer().name("dst_dense_vector")] __device__(int i) mutable
               {
                   auto index                   = unique_indices(i);
                   dst.segment<T, N>(index * N) = unique_segments(i);
               });
}

template <typename T>
void bsr2csr(int                         mb,
             int                         nb,
             int                         blockDim,
             cusparseMatDescr_t          descrA,
             const double*               bsrValA,
             const int*                  bsrRowPtrA,
             const int*                  bsrColIndA,
             int                         nnzb,
             DeviceCSRMatrix<T>&         to,
             muda::DeviceBuffer<int>&    row_offsets,
             muda::DeviceBuffer<int>&    col_indices,
             muda::DeviceBuffer<double>& values)
{
    using namespace muda;
    auto                handle = LinearSystemContext::current().cusparse();
    cusparseDirection_t dir    = CUSPARSE_DIRECTION_COLUMN;
    int                 m      = mb * blockDim;
    int                 nnz = nnzb * blockDim * blockDim;  // number of elements
    to.reshape(m, m);
    col_indices.resize(nnz);
    values.resize(nnz);
    checkCudaErrors(cusparseDbsr2csr(handle,
                                     dir,
                                     mb,
                                     nb,
                                     descrA,
                                     bsrValA,
                                     bsrRowPtrA,
                                     bsrColIndA,
                                     blockDim,
                                     to.legacy_descr(),
                                     values.data(),
                                     row_offsets.data(),
                                     col_indices.data()));
}


template <typename T, int N>
void MatrixFormatConverter<T, N>::convert(const DeviceBCOOMatrix<T, N>& from,
                                          DeviceCOOMatrix<T>&           to)
{
    expand_blocks(from, to);
    sort_indices_and_values(from, to);
}

template <typename T, int N>
void MatrixFormatConverter<T, N>::expand_blocks(const DeviceBCOOMatrix<T, N>& from,
                                                DeviceCOOMatrix<T>& to)
{
    using namespace muda;

    constexpr int N2 = N * N;

    to.reshape(from.block_rows() * N, from.block_cols() * N);
    to.resize_triplets(from.non_zero_blocks() * N2);

    auto& row_indices = to.m_row_indices;
    auto& col_indices = to.m_col_indices;
    auto& values      = to.m_values;

    auto& block_row_indices = from.m_block_row_indices;
    auto& block_col_indices = from.m_block_col_indices;
    auto& block_values      = from.m_block_values;


    ParallelFor(256)
        .kernel_name("set coo matrix")
        .apply(block_row_indices.size(),
               [block_row_indices = block_row_indices.cviewer().name("block_row_indices"),
                block_col_indices = block_col_indices.cviewer().name("block_col_indices"),
                block_values = block_values.cviewer().name("block_values"),
                row_indices  = row_indices.viewer().name("row_indices"),
                col_indices  = col_indices.viewer().name("col_indices"),
                values = values.viewer().name("values")] __device__(int i) mutable
               {
                   auto block_row_index = block_row_indices(i);
                   auto block_col_index = block_col_indices(i);
                   auto block           = block_values(i);

                   auto row = block_row_index * N;
                   auto col = block_col_index * N;

                   auto index = i * N2;
#pragma unroll
                   for(int r = 0; r < N; ++r)
                   {
#pragma unroll
                       for(int c = 0; c < N; ++c)
                       {
                           row_indices(index) = row + r;
                           col_indices(index) = col + c;
                           values(index)      = block(r, c);
                           ++index;
                       }
                   }
               });
}

template <typename T, int N>
void MatrixFormatConverter<T, N>::sort_indices_and_values(const DeviceBCOOMatrix<T, N>& from,
                                                          DeviceCOOMatrix<T>& to)
{
    using namespace muda;

    auto& row_indices = to.m_row_indices;
    auto& col_indices = to.m_col_indices;
    auto& values      = to.m_values;

    ij_pairs.resize(row_indices.size());

    ParallelFor(256)
        .kernel_name("set ij pairs")
        .apply(row_indices.size(),
               [row_indices = row_indices.cviewer().name("row_indices"),
                col_indices = col_indices.cviewer().name("col_indices"),
                ij_pairs = ij_pairs.viewer().name("ij_pairs")] __device__(int i) mutable
               {
                   ij_pairs(i).x = row_indices(i);
                   ij_pairs(i).y = col_indices(i);
               });

    DeviceMergeSort().SortPairs(workspace,
                                ij_pairs.data(),
                                to.m_values.data(),
                                ij_pairs.size(),
                                [] __device__(const int2& a, const int2& b) {
                                    return a.x < b.x || (a.x == b.x && a.y < b.y);
                                });

    // set ij_pairs back to row_indices and col_indices

    auto dst_row_indices = to.row_indices();
    auto dst_col_indices = to.col_indices();

    ParallelFor(256)
        .kernel_name("set col row indices")
        .apply(dst_row_indices.size(),
               [row_indices = dst_row_indices.viewer().name("row_indices"),
                col_indices = dst_col_indices.viewer().name("col_indices"),
                ij_pairs = ij_pairs.viewer().name("ij_pairs")] __device__(int i) mutable
               {
                   row_indices(i) = ij_pairs(i).x;
                   col_indices(i) = ij_pairs(i).y;
               });
}

template <typename T, int N>
void MatrixFormatConverter<T, N>::convert(const DeviceBSRMatrix<T, N>& from,
                                          DeviceCSRMatrix<T>&          to)
{
    using namespace muda;

    bsr2csr(from.block_rows(),
            from.block_cols(),
            N,
            from.legacy_descr(),
            (const double*)from.m_block_values.data(),
            from.m_block_row_offsets.data(),
            from.m_block_col_indices.data(),
            from.non_zeros(),
            to,
            to.m_row_offsets,
            to.m_col_indices,
            to.m_values);
}
}  // namespace muda::details