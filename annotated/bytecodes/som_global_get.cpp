#include "api_define_bytecode.h"
#include "deegen_api.h"
#include "api_inline_cache.h"

#include "runtime_utils.h"
#include "som_utils.h"

static void NO_RETURN UnknownGlobalSlowPathContinuation(TValue /*self*/, uint16_t /*index*/)
{
    Return(GetReturnValue(0));
}

static void NO_RETURN UnknownGlobalSlowPath(TValue self, uint16_t index)
{
    VM* vm = VM_GetActiveVMForCurrentThread();
    TestAssert(index < vm->m_globalStringIdWithIndex.size());
    HeapPtr<SOMClass> cl = GetSOMClassOfAny(self);
    GeneralHeapPointer<FunctionObject> func = SOMClass::GetMethod(cl, vm->m_unknownGlobalHandler);
    // This shouldn't fail since Object defined unknown global handler and everything inherits from Object
    //
    TestAssert(func.m_value != 0);
    SOMObject* globalName = vm->GetInternedSymbol(vm->m_globalStringIdWithIndex[index]);
    TValue gn = TValue::Create<tObject>(TranslateToHeapPtr(globalName));
    MakeCall(func.As(), self, gn, UnknownGlobalSlowPathContinuation);
}

static void NO_RETURN GlobalGetImpl(TValue /*self*/, uint16_t index)
{
    TValue val = VM::VM_GetGlobal(index);
    if (unlikely(val.m_value == TValue::CreateImpossibleValue().m_value))
    {
        EnterSlowPath<UnknownGlobalSlowPath>();
    }
    Return(val);
}

DEEGEN_DEFINE_BYTECODE(SOMGlobalGet)
{
    Operands(
        BytecodeSlot("self"),
        Literal<uint16_t>("index")
    );
    Result(BytecodeValue);
    Implementation(GlobalGetImpl);
    Variant();
    DfgVariant();
    TypeDeductionRule(ValueProfile);
}

DEEGEN_END_BYTECODE_DEFINITIONS
