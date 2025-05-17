#include "api_define_bytecode.h"
#include "deegen_api.h"
#include "som_class.h"
#include "som_utils.h"

#include "runtime_utils.h"

// This could have been specialized to make common cases faster but it seems like the benchmarks do not use this.
//
static void NO_RETURN ArrayDupImpl(TValue src)
{
    Return(DeepCloneConstantArray(src));
}

DEEGEN_DEFINE_BYTECODE(SOMArrayDup)
{
    Operands(
        Constant("src")
    );
    Result(BytecodeValue);
    Implementation(ArrayDupImpl);
    Variant(
        Op("src").IsConstant<tObject>()
    );
    DfgVariant();
    TypeDeductionRule(AlwaysOutput<tObject>);
    RegAllocHint(
        Op("src").RegHint(RegHint::GPR),
        Op("output").RegHint(RegHint::GPR)
    );
}

DEEGEN_END_BYTECODE_DEFINITIONS
