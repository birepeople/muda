#pragma once
#include <muda/buffer/buffer_view.h>
#include <muda/compute_graph/compute_graph_var.h>

namespace muda
{
template <typename T>
class ComputeGraphVar<BufferView<T>> : public ComputeGraphVarBase
{
  public:
    static_assert(!std::is_const_v<T>, "T must not be const");
    using VarType = BufferView<T>;
    using ROView  = read_only_viewer_t<VarType>;
    using RWView  = VarType;

  protected:
    friend class ComputeGraph;
    friend class ComputeGraphVarManager;

    using ComputeGraphVarBase::ComputeGraphVarBase;

    ComputeGraphVar(ComputeGraphVarManager* var_manager, std::string_view name, VarId var_id) MUDA_NOEXCEPT
        : ComputeGraphVarBase(var_manager, name, var_id)
    {
    }

    ComputeGraphVar(ComputeGraphVarManager* var_manager,
                    std::string_view        name,
                    VarId                   var_id,
                    const T&                init_value) MUDA_NOEXCEPT
        : ComputeGraphVarBase(var_manager, name, var_id, true),
          m_value(init_value)
    {
    }

    virtual ~ComputeGraphVar() = default;

  public:
    ROView ceval() const { return _ceval(m_value); }
    RWView eval() { return _eval(m_value); }
    operator ROView() const { return ceval(); }
    operator RWView() { return eval(); }
    auto cviewer() const { return ceval().cviewer(); };
    auto viewer() { return eval().viewer(); };

    void                      update(const RWView& view);
    ComputeGraphVar<VarType>& operator=(const RWView& view);

  private:
    RWView m_value;
};

}  // namespace muda

#include "details/graph_buffer_view.inl"