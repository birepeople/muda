#include <catch2/catch.hpp>
#include <muda/muda.h>

using namespace muda;

struct kernelA
{
    kernelA(const muda::idxer1D<int>& s, const idxer<int>& var)
        : s(s)
        , var(var)
    {
    }

  public:
    idxer1D<int>    s;
    idxer<int>      var;
    __device__ void operator()(int i) { var = s.total_size(); }
};

// setKernelNodeParms
void mem_realloc(int first, int last, int& outfirst, int& outlast)
{
    using namespace muda;
    // alloc device memory
    auto s   = device_vector<int>(first);
    auto var = device_var<int>(0);

    // create kernel
    kernelA a(make_viewer(s), make_viewer(var));

    // create graph
    auto graph = graph::create();

    // setup graph node
    auto pA = parallel_for(1).asNodeParms(1, a);
    auto kA = graph->addKernelNode(pA);
    // create graph instance
    auto instance = graph->instantiate();

    // launch graph
    instance->launch();
    launch::wait_device();
    outfirst = var;

    // realloc some device memory
    s.resize(last);
    // reset node parameters
    a.s = make_viewer(s);
    pA  = parallel_for(1).asNodeParms(1, a);
    instance->setKernelNodeParms(kA, pA);
    // luanch again
    instance->launch();
    launch::wait_device();
    outlast = var;
}

TEST_CASE("set_graphExec_node_parms", "[graph]")
{
    for(int i = 10, j = 100; i < 100 && j < 1000; i += 10, j += 100)
    {
        std::stringstream ss;
        ss << "resize " << i << "->" << j;
        SECTION(ss.str())
        {
            int res_i, res_j;
            mem_realloc(i, j, res_i, res_j);
            REQUIRE(i == res_i);
            REQUIRE(j == res_j);
        }
    }
}

// graph alloc
void alloc_cpy_free(int half, host_vector<int>& host_data, host_vector<int>& ground_thruth)
{
    auto count = half * 2;
    ground_thruth.resize(count);
    host_data.resize(count);
    for(int i = 0; i < ground_thruth.size(); i++)
    {
        ground_thruth[i] = i % half;
    }

    cudaStream_t stream;
    cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
    auto hostIdxer = make_viewer(host_data);

    auto  allocParm = memory::asAllocNodeParms<int>(count);
    graph g;
    auto [allocNode, ptr] = g.addMemAllocNode(allocParm);

    auto idxer = make_viewer(ptr, count);

    auto writeKernelParm = parallel_for(2, 8).asNodeParms(
        count, [idxer = idxer] __device__(int i) mutable { idxer(i) = i; });
    auto writeKernelNode = g.addKernelNode(writeKernelParm, {allocNode});
    auto readKernelParm  = launch(1, 1).asNodeParms(
        [idxer = idxer] __device__() mutable
        {
            for(int i = 0; i < idxer.total_size(); ++i)
                ;
            //printf("%d\n", idxer(i));
        });
    auto readKernelNode = g.addKernelNode(readKernelParm, {writeKernelNode});
    auto cpyNode        = g.addMemcpyNode(
        hostIdxer.data(), idxer.data(), half, cudaMemcpyDeviceToHost, {readKernelNode});
    auto freeNode = g.addMemFreeNode(allocNode, {cpyNode});
    auto instance = g.instantiate();
    instance->launch(stream);
    launch::wait_stream(stream);
    auto hostIdxerHalf = idxer1D<int>(hostIdxer.data() + half, half);
    instance->setMemcpyNodeParms(cpyNode, hostIdxerHalf.data(), idxer.data(), half, cudaMemcpyDeviceToHost);
    instance->launch(stream);
    launch::wait_stream(stream);

    launch::wait_device();
}

TEST_CASE("graph alloc copy free kernel node test", "[graph]")
{
    host_vector<int> v;
    host_vector<int> gt;
    for(int i = 50; i <= 1000; i += 50)
    {
        SECTION(std::to_string(i).c_str())
        {
            alloc_cpy_free(i, v, gt);
            REQUIRE(v == gt);
        }
    }
}


