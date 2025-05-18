#include "api_define_bytecode.h"
#include "deegen_api.h"

#include "runtime_utils.h"
#include "vm.h"

static void NO_RETURN ReturnImpl(const TValue* retStart, uint16_t /*numRet*/)
{
    GuestLanguageFunctionReturn(retStart, 1);
}

DEEGEN_DEFINE_BYTECODE(Ret)
{
    Operands(
        BytecodeRangeBaseRO("retStart"),
        Literal<uint16_t>("numRet")
    );
    Result(NoOutput);
    Implementation(ReturnImpl);
    Variant(Op("numRet").HasValue(1));
    DfgVariant();
    DeclareReads(Range(Op("retStart"), Op("numRet")));
    DeclareAsIntrinsic<Intrinsic::FunctionReturn>({
        .start = Op("retStart"),
        .length = Op("numRet")
    });
}

#ifdef ENABLE_SOM_PROFILE_FREQUENCY

static void NO_RETURN ProfileFrequencyImpl(TValue value)
{
    Assert(value.Is<tInt32>());
    int32_t idx = value.As<tInt32>();
    Assert(idx >= 0);
    VM* vm = VM::GetActiveVMForCurrentThread();
    vm->m_methCallCountArr[idx]++;
    Return();
}

DEEGEN_DEFINE_BYTECODE(SOMProfileCallFreq)
{
    Operands(
        Constant("idx")
    );
    Result(NoOutput);
    Implementation(ProfileFrequencyImpl);
    Variant();
    DfgVariant();
}

#endif  // ifdef ENABLE_SOM_PROFILE_FREQUENCY

DEEGEN_END_BYTECODE_DEFINITIONS
