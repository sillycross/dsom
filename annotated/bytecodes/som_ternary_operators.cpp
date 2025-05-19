#include "api_define_bytecode.h"
#include "deegen_api.h"
#include "api_inline_cache.h"

#include "runtime_utils.h"
#include "som_class.h"
#include "som_utils.h"
#include "som_call_utils.h"

// All specializable ternary operators
//
enum class TernaryOpKind
{
    AtPut,        // at:put:
    ValueWith,    // value:with:
};

// Below is the fallback generic slow path logic that correctly but slowly implements any ternary operator
//
static void NO_RETURN TernaryOpCallReturnContinuation(TValue /*op*/, TValue /*arg1*/, TValue /*arg2*/)
{
    Return(GetReturnValue(0));
}

static void NO_RETURN TernaryOpCallMethodNotFoundSlowPath(TValue op, TValue arg1, TValue arg2, SOMUniquedString meth)
{
    VM* vm = VM_GetActiveVMForCurrentThread();
    HeapPtr<SOMClass> cl = GetSOMClassOfAny(op);
    GeneralHeapPointer<FunctionObject> handler = SOMClass::GetMethod(cl, vm->m_doesNotUnderstandHandler);
    TestAssert(handler.m_value != 0);
    TValue fnName = TValue::Create<tObject>(TranslateToHeapPtr(vm->GetInternedSymbol(meth.m_id)));
    SOMObject* args = SOMObject::AllocateArray(2 /*numArgs*/);
    args->m_data[1] = arg1;
    args->m_data[2] = arg2;
    MakeCall(handler.As(), op, fnName, TValue::Create<tObject>(TranslateToHeapPtr(args)), TernaryOpCallReturnContinuation);
}

template<TernaryOpKind kind>
static SOMUniquedString ALWAYS_INLINE WARN_UNUSED GetLookupKeyForTernaryOperator()
{
    VM* vm = VM_GetActiveVMForCurrentThread();
    switch (kind)
    {
    case TernaryOpKind::AtPut: return vm->m_strOperatorAtPut;
    case TernaryOpKind::ValueWith: return vm->m_strOperatorValueWith;
    }   /*switch*/
    __builtin_unreachable();
}

template<TernaryOpKind kind>
static void NO_RETURN TernaryOpGeneralSlowPath(TValue op, TValue arg1, TValue arg2)
{
    HeapPtr<SOMClass> cl = GetSOMClassOfAny(op);
    SOMUniquedString meth = GetLookupKeyForTernaryOperator<kind>();
    GeneralHeapPointer<FunctionObject> f = SOMClass::GetMethod(cl, meth);
    if (f.m_value == 0)
    {
        EnterSlowPath<TernaryOpCallMethodNotFoundSlowPath>(meth);
    }
    MakeCall(f.As(), op, arg1, arg2, TernaryOpCallReturnContinuation);
}

static void NO_RETURN ArrayWriteOutOfBoundSlowPath(TValue op, TValue arg1, TValue /*arg2*/)
{
    int32_t idx = arg1.As<tInt32>();
    HeapPtr<SOMObject> o = op.As<tObject>();
    fprintf(stderr, "Array write out of bound: index = %d, size = %d\n",
            static_cast<int>(idx), static_cast<int>(o->m_data[0].m_value));
    abort();
}

template<TernaryOpKind kind>
static void NO_RETURN MiscTernaryOpImpl(TValue op, TValue arg1, TValue arg2)
{
    if constexpr(kind == TernaryOpKind::AtPut)
    {
        if (likely(op.Is<tHeapEntity>() && op.As<tHeapEntity>()->m_arrayType == SOM_Array))
        {
            int32_t idx = arg1.As<tInt32>();
            HeapPtr<SOMObject> o = op.As<tObject>();
            if (unlikely(static_cast<uint32_t>(idx) - 1 >= o->m_data[0].m_value))
            {
                EnterSlowPath<ArrayWriteOutOfBoundSlowPath>();
            }
            o->m_data[idx].m_value = arg2.m_value;
            Return(op);
        }
    }
    else
    {
        static_assert(kind == TernaryOpKind::ValueWith);
        if (likely(op.Is<tHeapEntity>()))
        {
            SOMDetailEntityType fnTy = static_cast<SOMDetailEntityType>(op.As<tHeapEntity>()->m_arrayType & 15);
            if (likely(fnTy == SOM_BlockTwoArgs || fnTy == SOM_BlockTwoArgsImmSelf))
            {
                TValue self;
                HeapPtr<FunctionObject> fn = op.As<tFunction>();
                if (fnTy == SOM_BlockTwoArgsImmSelf)
                {
                    self.m_value = fn->m_upvalues[0].m_value;
                }
                else
                {
                    self = *reinterpret_cast<Upvalue*>(fn->m_upvalues[0].m_value)->m_ptr;
                }
                MakeCall(fn, self, arg1, arg2, TernaryOpCallReturnContinuation);
            }
        }
    }
    EnterSlowPath<TernaryOpGeneralSlowPath<kind>>();
}

DEEGEN_DEFINE_BYTECODE_TEMPLATE(MiscTernaryOp, TernaryOpKind kind)
{
    Operands(
        BytecodeSlot("op"),
        BytecodeSlotOrConstant("arg1"),
        BytecodeSlotOrConstant("arg2")
    );
    Result(BytecodeValue);
    Implementation(MiscTernaryOpImpl<kind>);
    Variant(Op("arg1").IsBytecodeSlot(), Op("arg2").IsBytecodeSlot());
    Variant(Op("arg1").IsBytecodeSlot(), Op("arg2").IsConstant());
    Variant(Op("arg1").IsConstant(), Op("arg2").IsBytecodeSlot());
    Variant(Op("arg1").IsConstant(), Op("arg2").IsConstant());
    DfgVariant();
    TypeDeductionRule(ValueProfile);
}

DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorAtPut, MiscTernaryOp, TernaryOpKind::AtPut);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorValueWith, MiscTernaryOp, TernaryOpKind::ValueWith);

DEEGEN_END_BYTECODE_DEFINITIONS
