#include "api_define_bytecode.h"
#include "deegen_api.h"
#include "api_inline_cache.h"

#include "runtime_utils.h"
#include "som_class.h"

static void NO_RETURN GetFieldImpl(TValue base, uint16_t index)
{
    Assert(base.Is<tObject>());
    HeapPtr<SOMObject> o = base.As<tObject>();
    Return(TCGet(o->m_data[index]));
}

DEEGEN_DEFINE_BYTECODE(SOMGetField)
{
    Operands(
        BytecodeSlot("base"),
        Literal<uint16_t>("index")
    );
    Result(BytecodeValue);
    Implementation(GetFieldImpl);
    Variant();
    DfgVariant();
    TypeDeductionRule(ValueProfile);
}

static void NO_RETURN PutFieldImpl(TValue base, uint16_t index, TValue value)
{
    Assert(base.Is<tObject>());
    HeapPtr<SOMObject> o = base.As<tObject>();
    TCSet(o->m_data[index], value);
    Return();
}

DEEGEN_DEFINE_BYTECODE(SOMPutField)
{
    Operands(
        BytecodeSlot("base"),
        Literal<uint16_t>("index"),
        BytecodeSlotOrConstant("value")
    );
    Result(NoOutput);
    Implementation(PutFieldImpl);
    Variant(Op("value").IsBytecodeSlot());
    Variant(Op("value").IsConstant());
    DfgVariant();
}

DEEGEN_END_BYTECODE_DEFINITIONS
