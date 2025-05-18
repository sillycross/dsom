#pragma once

#include "deegen_options.h"
#include "misc_llvm_helper.h"
#include "deegen_register_pinning_scheme.h"

namespace dast
{

class DeegenLibFuncInstance
{
    MAKE_NONCOPYABLE(DeegenLibFuncInstance);
    MAKE_NONMOVABLE(DeegenLibFuncInstance);

public:
    // Note that the input 'target' is the dummy declaration, and this constructor will change
    // it to the true definition, and as a result, any pointer to 'target' will be invalidated
    //
    DeegenLibFuncInstance(llvm::Function* impl, llvm::Function* target, bool isRc);

    // This function should be called after a PerFunctionSimplifyOnly desugaring pass on the module
    //
    void DoLowering();

    llvm::Module* GetModule() const { return m_module; }
    llvm::Value* GetCoroutineCtx() const { return m_valuePreserver.Get(x_coroutineCtx); }
    llvm::Value* GetStackBase() const { return m_valuePreserver.Get(x_stackBase); }
    llvm::Value* GetNumArgs() const { ReleaseAssert(!m_isReturnContinuation); return m_valuePreserver.Get(x_numArgs); }

    ExecutorFunctionContext* GetFuncContext()
    {
        ReleaseAssert(m_funcCtx.get() != nullptr);
        return m_funcCtx.get();
    }

private:
    static constexpr const char* x_coroutineCtx = "coroutineCtx";
    static constexpr const char* x_stackBase = "stackBase";
    static constexpr const char* x_numArgs = "numArgs";

    LLVMValuePreserver m_valuePreserver;
    std::unique_ptr<ExecutorFunctionContext> m_funcCtx;
    llvm::Module* m_module;
    llvm::Function* m_impl;
    llvm::Function* m_target;
    bool m_isReturnContinuation;
};

struct DeegenLibFuncInstanceInfo
{
    std::string m_implName;
    std::string m_wrapperName;
    bool m_isRc;
};

struct DeegenLibFuncProcessor
{
    static std::vector<DeegenLibFuncInstanceInfo> WARN_UNUSED ParseInfo(llvm::Module* module);
    static void DoLowering(llvm::Module* module);

    static constexpr const char* x_allDefsHolderSymbolName = "x_deegen_impl_all_lib_func_defs_in_this_tu";
};

}   // namespace dast

