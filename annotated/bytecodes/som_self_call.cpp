#include "api_define_bytecode.h"
#include "deegen_api.h"
#include "api_inline_cache.h"

#include "runtime_utils.h"
#include "som_class.h"
#include "som_utils.h"
#include "som_call_utils.h"

static void NO_RETURN SelfCallReturnContinuation(TValue* /*base*/, TValue /*self*/, TValue /*methTv*/, uint16_t /*numArgs*/)
{
    Return(GetReturnValue(0));
}

static void NO_RETURN SelfCallMethodNotFoundSlowPath(TValue* base, TValue self, TValue methTv, uint16_t numArgs)
{
    HandleMethodNotFoundImpl<SelfCallReturnContinuation>(self, methTv, base + x_numSlotsForStackFrameHeader + 1, numArgs - 1);
}

static void NO_RETURN SelfCallImpl(TValue* base, TValue self, TValue methTv, uint16_t numArgs)
{
    // The bytecode generator takes care to only emit self-call if 'self' is an SOMObject, so no need to check.
    //
    Assert(self.Is<tObject>());
    auto [fnKind, fn] = LookupObjectMethodImpl<false /*isSuper*/>(self.As<tHeapEntity>(), methTv);
    switch (fnKind)
    {
    case SOM_MethodNotFound:
    {
        EnterSlowPath<SelfCallMethodNotFoundSlowPath>();
    }
    case SOM_CallBaseNotObject:
    {
        Assert(false);
        __builtin_unreachable();
    }
    case SOM_NormalMethod:
    {
        base[x_numSlotsForStackFrameHeader] = self;
        MakeInPlaceCall(fn, base + x_numSlotsForStackFrameHeader, numArgs, SelfCallReturnContinuation);
    }
    case SOM_Setter:
    {
        Return(ExecuteSetterTrivialMethod(fn, self, base[x_numSlotsForStackFrameHeader + 1]));
    }
    default:
    {
        Return(ExecuteTrivialMethodExceptSetter(fn, self, fnKind));
    }
    } /*switch*/
}

DEEGEN_DEFINE_BYTECODE(SOMSelfCall)
{
    Operands(
        BytecodeRangeBaseRW("base"),
        BytecodeSlot("self"),
        Constant("meth"),
        Literal<uint16_t>("numArgs")
    );
    Result(BytecodeValue);
    Implementation(SelfCallImpl);
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
