#include "api_define_bytecode.h"
#include "deegen_api.h"
#include "api_inline_cache.h"

#include "runtime_utils.h"
#include "som_class.h"

static void NO_RETURN SOMThrowImpl(TValue value)
{
    ThrowError(value);
}

DEEGEN_DEFINE_BYTECODE(SOMThrow)
{
    Operands(
        BytecodeSlot("value")
    );
    Result(NoOutput);
    Implementation(SOMThrowImpl);
    Variant();
    DfgVariant();
}

DEEGEN_END_BYTECODE_DEFINITIONS
