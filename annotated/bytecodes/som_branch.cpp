#include "api_define_bytecode.h"
#include "deegen_api.h"

template<bool ifTrue>
static void NO_RETURN BranchIfTrueOrFalseImpl(TValue cond)
{
    bool shouldBranch = (ifTrue ? (cond.m_value == TValue::Create<tBool>(true).m_value) : (cond.m_value == TValue::Create<tBool>(false).m_value));
    if (shouldBranch)
    {
        ReturnAndBranch();
    }
    else
    {
        Return();
    }
}

DEEGEN_DEFINE_BYTECODE_TEMPLATE(BranchIfTrueOrFalse, bool ifTrue)
{
    Operands(BytecodeSlot("cond"));
    Result(ConditionalBranch);
    Implementation(BranchIfTrueOrFalseImpl<ifTrue>);
    Variant();
    DfgVariant();
}

DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(BranchIfFalse, BranchIfTrueOrFalse, false /*ifTrue*/);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(BranchIfTrue, BranchIfTrueOrFalse, true /*ifTrue*/);

template<bool ifTrue>
static void NO_RETURN CopyAndBranchIfTrueOrFalseImpl(TValue cond)
{
    bool shouldBranch = (ifTrue ? (cond.m_value == TValue::Create<tBool>(true).m_value) : (cond.m_value == TValue::Create<tBool>(false).m_value));
    if (shouldBranch)
    {
        ReturnAndBranch(cond);
    }
    else
    {
        Return(cond);
    }
}

DEEGEN_DEFINE_BYTECODE_TEMPLATE(CopyAndBranchIfTrueOrFalse, bool ifTrue)
{
    Operands(BytecodeSlot("cond"));
    Result(BytecodeValue, ConditionalBranch);
    Implementation(CopyAndBranchIfTrueOrFalseImpl<ifTrue>);
    Variant();
    DfgVariant();
    TypeDeductionRule(ValueProfile);
}

DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(CopyAndBranchIfFalse, CopyAndBranchIfTrueOrFalse, false /*ifTrue*/);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(CopyAndBranchIfTrue, CopyAndBranchIfTrueOrFalse, true /*ifTrue*/);

template<bool ifTrue>
static void NO_RETURN StoreNilAndBranchIfTrueOrFalseImpl(TValue cond)
{
    bool shouldBranch = (ifTrue ? (cond.m_value == TValue::Create<tBool>(true).m_value) : (cond.m_value == TValue::Create<tBool>(false).m_value));
    if (shouldBranch)
    {
        ReturnAndBranch(TValue::Create<tNil>());
    }
    else
    {
        Return(TValue::Create<tNil>());
    }
}

DEEGEN_DEFINE_BYTECODE_TEMPLATE(StoreNilAndBranchIfTrueOrFalse, bool ifTrue)
{
    Operands(BytecodeSlot("cond"));
    Result(BytecodeValue, ConditionalBranch);
    Implementation(StoreNilAndBranchIfTrueOrFalseImpl<ifTrue>);
    Variant();
    DfgVariant();
    TypeDeductionRule([](TypeMask /*cond*/) -> TypeMask { return x_typeMaskFor<tNil>; });
}

DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(StoreNilAndBranchIfFalse, StoreNilAndBranchIfTrueOrFalse, false /*ifTrue*/);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(StoreNilAndBranchIfTrue, StoreNilAndBranchIfTrueOrFalse, true /*ifTrue*/);

template<bool ifNot>
static void NO_RETURN BranchIfNilOrNotNilImpl(TValue cond)
{
    bool shouldBranch = (cond.m_value == TValue::Create<tNil>().m_value);
    if (ifNot) { shouldBranch = !shouldBranch; }
    if (shouldBranch)
    {
        ReturnAndBranch();
    }
    else
    {
        Return();
    }
}

DEEGEN_DEFINE_BYTECODE_TEMPLATE(BranchIfNilOrNotNil, bool ifNot)
{
    Operands(
        BytecodeSlot("cond")
    );
    Result(ConditionalBranch);
    Implementation(BranchIfNilOrNotNilImpl<ifNot>);
    Variant();
    DfgVariant();
}

DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(BranchIfNotNil, BranchIfNilOrNotNil, true /*ifNot*/);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(BranchIfNil, BranchIfNilOrNotNil, false /*ifNot*/);

template<bool ifNot>
static void NO_RETURN CopyAndBranchIfNilOrNotNilImpl(TValue cond)
{
    bool shouldBranch = (cond.m_value == TValue::Create<tNil>().m_value);
    if (ifNot) { shouldBranch = !shouldBranch; }
    if (shouldBranch)
    {
        ReturnAndBranch(cond);
    }
    else
    {
        Return(cond);
    }
}

DEEGEN_DEFINE_BYTECODE_TEMPLATE(CopyAndBranchIfNilOrNotNil, bool ifNot)
{
    Operands(
        BytecodeSlot("cond")
    );
    Result(BytecodeValue, ConditionalBranch);
    Implementation(CopyAndBranchIfNilOrNotNilImpl<ifNot>);
    Variant();
    DfgVariant();
    TypeDeductionRule(ValueProfile);
}

DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(CopyAndBranchIfNotNil, CopyAndBranchIfNilOrNotNil, true /*ifNot*/);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(CopyAndBranchIfNil, CopyAndBranchIfNilOrNotNil, false /*ifNot*/);

// Branch if the statement block should NOT be executed
//
template<bool forDownto>
static void NO_RETURN CheckForLoopStartCondImpl(TValue val, TValue limit)
{
    bool passed;
    if (val.Is<tInt32>())
    {
        // Assuming 'limit' has the same type as 'val' is clearly problematic but this is really what SOM++ did.
        //
        TestAssert(limit.Is<tInt32>());
        passed = (forDownto ? (val.As<tInt32>() >= limit.As<tInt32>()) : (val.As<tInt32>() <= limit.As<tInt32>()));
    }
    else
    {
        TestAssert(limit.Is<tDouble>());
        passed = (forDownto ? (val.As<tDouble>() >= limit.As<tDouble>()) : (val.As<tDouble>() <= limit.As<tDouble>()));
    }
    if (passed)
    {
        Return(val);
    }
    else
    {
        ReturnAndBranch(val);
    }
}

DEEGEN_DEFINE_BYTECODE_TEMPLATE(CheckForUpOrDownLoopStartCond, bool forDownto)
{
    Operands(
        BytecodeSlot("val"),
        BytecodeSlot("limit")
    );
    Result(BytecodeValue, ConditionalBranch);
    Implementation(CheckForLoopStartCondImpl<forDownto>);
    Variant();
    DfgVariant();
    TypeDeductionRule(ValueProfile);
}

DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(CheckForLoopStartCond, CheckForUpOrDownLoopStartCond, false /*forDownto*/);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(CheckDowntoForLoopStartCond, CheckForUpOrDownLoopStartCond, true /*forDownto*/);

// Branch if the statement block SHOULD be executed
//
template<bool forDownto>
static void NO_RETURN ForLoopStepImpl(TValue* base)
{
    // Assuming 'limit' is also integer is clearly problematic but this is really what SOM++ did.
    //
    TValue val = base[0];
    TValue limit = base[1];
    bool passed;
    if (val.Is<tInt32>())
    {
        // Assuming 'limit' has the same type as 'val' is clearly problematic but this is really what SOM++ did.
        //
        TestAssert(limit.Is<tInt32>());
        int32_t next = val.As<tInt32>();
        if (forDownto) { next--; } else { next++; }
        TValue nextTv = TValue::Create<tInt32>(next);
        base[0] = nextTv;
        base[2] = nextTv;
        passed = (forDownto ? (next >= limit.As<tInt32>()) : (next <= limit.As<tInt32>()));
    }
    else
    {
        TestAssert(limit.Is<tDouble>());
        double next = val.As<tDouble>();
        if (forDownto) { next -= 1.0; } else { next += 1.0; }
        TValue nextTv = TValue::Create<tDouble>(next);
        base[0] = nextTv;
        base[2] = nextTv;
        passed = (forDownto ? (next >= limit.As<tDouble>()) : (next <= limit.As<tDouble>()));
    }
    if (passed)
    {
        ReturnAndBranch();
    }
    else
    {
        Return();
    }
}

DEEGEN_DEFINE_BYTECODE_TEMPLATE(ForUpOrDownLoopStep, bool forDownto)
{
    Operands(
        BytecodeRangeBaseRW("base")
    );
    Result(ConditionalBranch);
    Implementation(ForLoopStepImpl<forDownto>);
    Variant();
    DfgVariant();
    CheckForInterpreterTierUp(true);
    DeclareReads(Range(Op("base"), 2));
    DeclareWrites(
        Range(Op("base"), 1).TypeDeductionRule(AlwaysOutput<tInt32>),
        Range(Op("base") + 2, 1).TypeDeductionRule(AlwaysOutput<tInt32>)
    );
}

DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(ForLoopStep, ForUpOrDownLoopStep, false /*forDownto*/);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(DowntoForLoopStep, ForUpOrDownLoopStep, true /*forDownto*/);

DEEGEN_END_BYTECODE_DEFINITIONS
