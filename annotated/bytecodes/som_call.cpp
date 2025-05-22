#include "api_define_bytecode.h"
#include "deegen_api.h"
#include "api_inline_cache.h"

#include "runtime_utils.h"
#include "som_class.h"
#include "som_utils.h"
#include "som_call_utils.h"

static void NO_RETURN CallReturnContinuation(TValue* /*base*/, TValue /*methTv*/, uint16_t /*numArgs*/)
{
    Return(GetReturnValue(0));
}

static void NO_RETURN CallMethodNotFoundSlowPath(TValue* base, TValue methTv, uint16_t numArgs)
{
    HandleMethodNotFoundImpl<CallReturnContinuation>(base[x_numSlotsForStackFrameHeader], methTv, base + x_numSlotsForStackFrameHeader + 1, numArgs - 1);
}

static void NO_RETURN CallBaseNotObjectSlowPath(TValue* base, TValue methTv, uint16_t numArgs)
{
    TValue self = base[x_numSlotsForStackFrameHeader];
    GeneralHeapPointer<FunctionObject> f = LookupMethodGeneralImpl(self, methTv);
    if (f.m_value == 0)
    {
        EnterSlowPath<CallMethodNotFoundSlowPath>();
    }
    MakeInPlaceCall(f.As(), base + x_numSlotsForStackFrameHeader, numArgs, CallReturnContinuation);
}

static void NO_RETURN CallImpl(TValue* base, TValue methTv, uint16_t numArgs)
{
    TValue self = base[x_numSlotsForStackFrameHeader];
    if (likely(self.Is<tHeapEntity>()))
    {
        auto [fnKind, fn] = LookupObjectMethodImpl<false /*isSuper*/>(self.As<tHeapEntity>(), methTv);
        switch (fnKind)
        {
        case SOM_MethodNotFound:
        {
            EnterSlowPath<CallMethodNotFoundSlowPath>();
        }
        case SOM_CallBaseNotObject:
        {
            EnterSlowPath<CallBaseNotObjectSlowPath>();
        }
        case SOM_NormalMethod:
        {
            MakeInPlaceCall(fn, base + x_numSlotsForStackFrameHeader, numArgs, CallReturnContinuation);
        }
        case SOM_Setter:
        {
            Return(ExecuteSetterTrivialMethod(fn, self, base[x_numSlotsForStackFrameHeader + 1]));
        }
        default:
        {
            Return(ExecuteTrivialMethodExceptSetter(fn, self, fnKind));
        }
        }   /*switch*/
    }
    else
    {
        EnterSlowPath<CallBaseNotObjectSlowPath>();
    }
}

DEEGEN_DEFINE_BYTECODE(SOMCall)
{
    Operands(
        BytecodeRangeBaseRW("base"),
        Constant("meth"),
        Literal<uint16_t>("numArgs")
    );
    Result(BytecodeValue);
    Implementation(CallImpl);
    Variant();
    DfgVariant();
    TypeDeductionRule(ValueProfile);
    DeclareReads(
        Range(Op("base"), 1),
        Range(Op("base") + x_numSlotsForStackFrameHeader, Op("numArgs"))
    );
    // This is unneeded but to make Deegen happy.. Deegen should to be fixed, but doesn't matter now..
    DeclareWrites(Range(Op("base"), 0).TypeDeductionRule(ValueProfile));
    DeclareUsedByInPlaceCall(Op("base"));
}

DEEGEN_END_BYTECODE_DEFINITIONS
