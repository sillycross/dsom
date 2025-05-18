#include "api_define_bytecode.h"
#include "deegen_api.h"
#include "api_inline_cache.h"

#include "runtime_utils.h"
#include "som_class.h"
#include "som_utils.h"
#include "som_call_utils.h"

static void NO_RETURN SuperCallReturnContinuation(TValue* /*base*/, TValue /*self*/, TValue /*scTv*/, TValue /*methTv*/, uint16_t /*numArgs*/)
{
    Return(GetReturnValue(0));
}

static void NO_RETURN SuperCallMethodNotFoundSlowPath(TValue* base, TValue self, TValue /*scTv*/, TValue methTv, uint16_t numArgs)
{
    HandleMethodNotFoundImpl<SuperCallReturnContinuation>(self, methTv, base + x_numSlotsForStackFrameHeader + 1, numArgs - 1);
}

static void NO_RETURN SuperCallImpl(TValue* base, TValue self, TValue scTv, TValue methTv, uint16_t numArgs)
{
    // For super-call, 'super' is never used in the standard library for classes where 'self' is not SOMObject,
    // and since we assume these non-object classes won't be inherited by user,
    // it's fine to assume 'self' is SOMObject
    //
    HeapPtr<SOMClass> superClass = SystemHeapPointer<SOMClass>(static_cast<uint32_t>(scTv.m_value)).As();
    Assert(self.Is<tObject>());
    auto [fnKind, fn] = LookupObjectMethodImpl<true /*isSuper*/>(self.As<tHeapEntity>(), methTv, superClass);
    switch (fnKind)
    {
    case SOM_MethodNotFound:
    {
        EnterSlowPath<SuperCallMethodNotFoundSlowPath>();
    }
    case SOM_CallBaseNotObject:
    {
        Assert(false);
        __builtin_unreachable();
    }
    case SOM_NormalMethod:
    case SOM_LiteralReturn:  // TODO: provide specialized impls
    case SOM_GlobalReturn:
    case SOM_Getter:
    case SOM_Setter:
    {
        base[x_numSlotsForStackFrameHeader] = self;
        MakeInPlaceCall(fn, base + x_numSlotsForStackFrameHeader, numArgs, SuperCallReturnContinuation);
    }
    } /*switch*/
}

DEEGEN_DEFINE_BYTECODE(SOMSuperCall)
{
    Operands(
        BytecodeRangeBaseRW("base"),
        BytecodeSlot("self"),
        Constant("superClass"),
        Constant("meth"),
        Literal<uint16_t>("numArgs")
    );
    Result(BytecodeValue);
    Implementation(SuperCallImpl);
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
