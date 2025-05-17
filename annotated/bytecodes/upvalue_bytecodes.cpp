#include "api_define_bytecode.h"
#include "deegen_api.h"

static void NO_RETURN MutableUpvalueGetImpl(uint16_t ord)
{
    Return(UpvalueAccessor::GetMutable(ord));
}

DEEGEN_DEFINE_BYTECODE(UpvalueGetMutable)
{
    Operands(
        Literal<uint16_t>("ord")
    );
    Result(BytecodeValue);
    Implementation(MutableUpvalueGetImpl);
    Variant();
    DfgVariant();
    TypeDeductionRule(ValueProfile);
    DeclareAsIntrinsic<Intrinsic::UpvalueGetMutable>({
        .ord = Op("ord")
    });
}

static void NO_RETURN ImmutableUpvalueGetImpl(uint16_t ord)
{
    Return(UpvalueAccessor::GetImmutable(ord));
}

DEEGEN_DEFINE_BYTECODE(UpvalueGetImmutable)
{
    Operands(
        Literal<uint16_t>("ord")
    );
    Result(BytecodeValue);
    Implementation(ImmutableUpvalueGetImpl);
    Variant();
    DfgVariant();
    TypeDeductionRule(ValueProfile);
    DeclareAsIntrinsic<Intrinsic::UpvalueGetImmutable>({
        .ord = Op("ord")
    });
}

// Parser needs to late-replace between the two bytecodes
//
DEEGEN_ADD_BYTECODE_SAME_LENGTH_CONSTRAINT(UpvalueGetMutable, UpvalueGetImmutable);

static void NO_RETURN UpvalueSetImpl(uint16_t ord, TValue valueToPut)
{
    UpvalueAccessor::Put(ord, valueToPut);
    Return();
}

DEEGEN_DEFINE_BYTECODE(UpvaluePut)
{
    Operands(
        Literal<uint16_t>("ord"),
        BytecodeSlotOrConstant("value")
    );
    Result(NoOutput);
    Implementation(UpvalueSetImpl);
    Variant(Op("value").IsBytecodeSlot());
    Variant(Op("value").IsConstant());
    DfgVariant();
    DeclareAsIntrinsic<Intrinsic::UpvaluePut>({
        .ord = Op("ord"),
        .value = Op("value")
    });
}

template<bool shouldBranch>
static void NO_RETURN UpvalueCloseImpl(const TValue* base)
{
    UpvalueAccessor::Close(base);
    if constexpr(shouldBranch)
    {
        ReturnAndBranch();
    }
    else
    {
        Return();
    }
}

// UpvalueClose performs a jump. 'isLoopHint' hints whether this jump is a loop back edge.
//
DEEGEN_DEFINE_BYTECODE_TEMPLATE(UpvalueCloseOperation, bool shouldBranch, bool isLoopHint)
{
    Operands(
        BytecodeRangeBaseRO("base")
    );
    Result(shouldBranch ? ConditionalBranch : NoOutput);
    Implementation(UpvalueCloseImpl<shouldBranch>);
    CheckForInterpreterTierUp(isLoopHint);
    Variant();
    DfgVariant();
    DeclareReads();
    DeclareAsIntrinsic<Intrinsic::UpvalueClose>({
        .start = Op("base")
    });
}

DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(UpvalueClose, UpvalueCloseOperation, false /*shouldBranch*/, false /*isLoopHint*/);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(UpvalueCloseBranch, UpvalueCloseOperation, true /*shouldBranch*/, false /*isLoopHint*/);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(UpvalueCloseLoopHint, UpvalueCloseOperation, true /*shouldBranch*/, true /*isLoopHint*/);

DEEGEN_END_BYTECODE_DEFINITIONS
