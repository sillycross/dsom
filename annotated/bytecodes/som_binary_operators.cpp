#include "api_define_bytecode.h"
#include "deegen_api.h"
#include "api_inline_cache.h"

#include "runtime_utils.h"
#include "som_class.h"
#include "som_utils.h"
#include "som_call_utils.h"

// All specializable binary operators
//
enum class BinOpKind
{
    // Arithmetic binary operators that are most likely working on integer and double values,
    // so worth providing integer/double fast paths for these cases.
    //
    Plus,           // +
    Minus,          // -
    Star,           // *
    SlashSlash,     // //
    Percent,        // %
    And,            // &
    Equal,          // =
    LessThan,       // <
    LessEqual,      // <=
    GreaterThan,    // >
    GreaterEqual,   // >=
    Unequal,        // <>
    TildeUnequal,   // ~=
    LeftShift,      // <<
    RightShift,     // >>>
    BitwiseXor,     // bitXor:
    // Other non-arithmetic common binary operators that are worth providing a fast path for
    //
    LogicalAnd,     // &&
    LogicalOr,      // ||
    KeywordAnd,     // and:
    KeywordOr,      // or:
    ValueColon,     // value:
    AtColon,        // at:
    CharAtColon,    // charAt:
};

// Below is the fallback generic slow path logic that correctly but slowly implements any binary operator
//
static void NO_RETURN BinOpCallReturnContinuation(TValue /*lhs*/, TValue /*rhs*/)
{
    Return(GetReturnValue(0));
}

static void NO_RETURN BinOpCallMethodNotFoundSlowPath(TValue lhs, TValue rhs, SOMUniquedString meth)
{
    VM* vm = VM_GetActiveVMForCurrentThread();
    HeapPtr<SOMClass> cl = GetSOMClassOfAny(lhs);
    GeneralHeapPointer<FunctionObject> handler = SOMClass::GetMethod(cl, vm->m_doesNotUnderstandHandler);
    TestAssert(handler.m_value != 0);
    TValue fnName = TValue::Create<tObject>(TranslateToHeapPtr(vm->GetInternedSymbol(meth.m_id)));
    SOMObject* args = SOMObject::AllocateArray(1 /*numArgs*/);
    args->m_data[1] = rhs;
    MakeCall(handler.As(), lhs, fnName, TValue::Create<tObject>(TranslateToHeapPtr(args)), BinOpCallReturnContinuation);
}

template<BinOpKind kind>
static SOMUniquedString ALWAYS_INLINE WARN_UNUSED GetLookupKeyForBinaryOperator()
{
    VM* vm = VM_GetActiveVMForCurrentThread();
    switch (kind)
    {
    case BinOpKind::Plus: return vm->m_strOperatorPlus;
    case BinOpKind::Minus: return vm->m_strOperatorMinus;
    case BinOpKind::Star: return vm->m_strOperatorStar;
    case BinOpKind::SlashSlash: return vm->m_strOperatorSlashSlash;
    case BinOpKind::Percent: return vm->m_strOperatorPercent;
    case BinOpKind::And: return vm->m_strOperatorAnd;
    case BinOpKind::Equal: return vm->m_strOperatorEqual;
    case BinOpKind::LessThan: return vm->m_strOperatorLessThan;
    case BinOpKind::LessEqual: return vm->m_strOperatorLessEqual;
    case BinOpKind::GreaterThan: return vm->m_strOperatorGreaterThan;
    case BinOpKind::GreaterEqual: return vm->m_strOperatorGreaterEqual;
    case BinOpKind::Unequal: return vm->m_strOperatorUnequal;
    case BinOpKind::TildeUnequal: return vm->m_strOperatorTildeUnequal;
    case BinOpKind::LeftShift: return vm->m_strOperatorLeftShift;
    case BinOpKind::RightShift: return vm->m_strOperatorRightShift;
    case BinOpKind::BitwiseXor: return vm->m_strOperatorBitwiseXor;
    case BinOpKind::LogicalAnd: return vm->m_strOperatorLogicalAnd;
    case BinOpKind::LogicalOr: return vm->m_strOperatorLogicalOr;
    case BinOpKind::KeywordAnd: return vm->m_strOperatorKeywordAnd;
    case BinOpKind::KeywordOr: return vm->m_strOperatorKeywordOr;
    case BinOpKind::ValueColon: return vm->m_strOperatorValueColon;
    case BinOpKind::AtColon: return vm->m_strOperatorAtColon;
    case BinOpKind::CharAtColon: return vm->m_strOperatorCharAtColon;
    }   /*switch*/
    __builtin_unreachable();
}

template<BinOpKind kind>
static void NO_RETURN BinOpGeneralSlowPath(TValue lhs, TValue rhs)
{
    HeapPtr<SOMClass> cl = GetSOMClassOfAny(lhs);
    SOMUniquedString meth = GetLookupKeyForBinaryOperator<kind>();
    GeneralHeapPointer<FunctionObject> f = SOMClass::GetMethod(cl, meth);
    if (f.m_value == 0)
    {
        EnterSlowPath<BinOpCallMethodNotFoundSlowPath>(meth);
    }
    MakeCall(f.As(), lhs, rhs, BinOpCallReturnContinuation);
}

static int32_t ALWAYS_INLINE DoSOMIntegerPercent(int32_t lhs, int32_t rhs)
{
    int32_t result = lhs % rhs;
    if ((result != 0) && ((result < 0) != (rhs < 0))) { result += rhs; }
    return result;
}

template<BinOpKind kind>
static void ALWAYS_INLINE DoIntegerIntegerArithOp(int32_t lhs, int32_t rhs)
{
    switch (kind)
    {
    case BinOpKind::Plus: { Return(TValue::Create<tInt32>(lhs + rhs)); }
    case BinOpKind::Minus: { Return(TValue::Create<tInt32>(lhs - rhs)); }
    case BinOpKind::Star: { Return(TValue::Create<tInt32>(lhs * rhs)); }
    case BinOpKind::SlashSlash: { Return(TValue::Create<tDouble>(static_cast<double>(lhs) / static_cast<double>(rhs))); }
    case BinOpKind::Percent: {Return(TValue::Create<tInt32>(DoSOMIntegerPercent(lhs, rhs))); }
    case BinOpKind::And: { Return(TValue::Create<tInt32>(lhs & rhs)); }
    case BinOpKind::Equal: { Return(TValue::Create<tBool>(lhs == rhs)); }
    case BinOpKind::LessThan: { Return(TValue::Create<tBool>(lhs < rhs)); }
    case BinOpKind::LessEqual: { Return(TValue::Create<tBool>(lhs <= rhs)); }
    case BinOpKind::GreaterThan: { Return(TValue::Create<tBool>(lhs > rhs)); }
    case BinOpKind::GreaterEqual: { Return(TValue::Create<tBool>(lhs >= rhs)); }
    case BinOpKind::Unequal: { Return(TValue::Create<tBool>(lhs != rhs)); }
    case BinOpKind::TildeUnequal: { Return(TValue::Create<tBool>(lhs != rhs)); }
    case BinOpKind::LeftShift: { Return(TValue::Create<tInt32>(lhs << rhs)); }
    case BinOpKind::RightShift: { Return(TValue::Create<tInt32>(lhs >> rhs)); }
    case BinOpKind::BitwiseXor: { Return(TValue::Create<tInt32>(lhs ^ rhs)); }
    default: { break; }     // not an arithmetic binary operator, shouldn't reach here
    }   /*switch*/
}

template<BinOpKind kind>
static void ALWAYS_INLINE DoIntegerDoubleArithOp(int32_t lhs, double rhs)
{
    switch (kind)
    {
    case BinOpKind::Plus: { Return(TValue::Create<tDouble>(lhs + rhs)); }
    case BinOpKind::Minus: { Return(TValue::Create<tDouble>(lhs - rhs)); }
    case BinOpKind::Star: { Return(TValue::Create<tDouble>(lhs * rhs)); }
    case BinOpKind::SlashSlash: { Return(TValue::Create<tDouble>(static_cast<double>(lhs) / rhs)); }
    case BinOpKind::Percent: { Return(TValue::Create<tInt32>(DoSOMIntegerPercent(lhs, static_cast<int32_t>(rhs)))); }
    case BinOpKind::And: { break; }    // this is undefined behavior (same as in SOM++), just fallback to slowpath to trigger that UB...
    case BinOpKind::Equal: { Return(TValue::Create<tBool>(UnsafeFloatEqual(static_cast<double>(lhs), rhs))); }
    case BinOpKind::LessThan: { Return(TValue::Create<tBool>(lhs < rhs)); }
    case BinOpKind::LessEqual: { Return(TValue::Create<tBool>(lhs <= rhs)); }
    case BinOpKind::GreaterThan: { Return(TValue::Create<tBool>(lhs > rhs)); }
    case BinOpKind::GreaterEqual: { Return(TValue::Create<tBool>(lhs >= rhs)); }
    case BinOpKind::Unequal: { Return(TValue::Create<tBool>(UnsafeFloatUnequal(static_cast<double>(lhs), rhs))); }
    case BinOpKind::TildeUnequal: { Return(TValue::Create<tBool>(UnsafeFloatUnequal(static_cast<double>(lhs), rhs))); }
    case BinOpKind::LeftShift: { break; }   // same as And, also below
    case BinOpKind::RightShift: { break; }
    case BinOpKind::BitwiseXor: { break; }
    default: { break; }     // not an arithmetic binary operator, shouldn't reach here
    }   /*switch*/
}

template<BinOpKind kind>
static void ALWAYS_INLINE DoDoubleArithOp(double lhs, double rhs)
{
    switch (kind)
    {
    case BinOpKind::Plus: { Return(TValue::Create<tDouble>(lhs + rhs)); }
    case BinOpKind::Minus: { Return(TValue::Create<tDouble>(lhs - rhs)); }
    case BinOpKind::Star: { Return(TValue::Create<tDouble>(lhs * rhs)); }
    case BinOpKind::SlashSlash: { Return(TValue::Create<tDouble>(lhs / rhs)); }
    case BinOpKind::Percent: { Return(TValue::Create<tDouble>(static_cast<double>(static_cast<int64_t>(lhs) % static_cast<int64_t>(rhs)))); }
    case BinOpKind::And: { break; }     // bitwiseAnd not defined for double, fallback to slow path (which will trigger doesNotUnderstand)
    case BinOpKind::Equal: { Return(TValue::Create<tBool>(UnsafeFloatEqual(lhs, rhs))); }
    case BinOpKind::LessThan: { Return(TValue::Create<tBool>(lhs < rhs)); }
    case BinOpKind::LessEqual: { Return(TValue::Create<tBool>(lhs <= rhs)); }
    case BinOpKind::GreaterThan: { Return(TValue::Create<tBool>(lhs > rhs)); }
    case BinOpKind::GreaterEqual: { Return(TValue::Create<tBool>(lhs >= rhs)); }
    case BinOpKind::Unequal: { Return(TValue::Create<tBool>(UnsafeFloatUnequal(lhs, rhs))); }
    case BinOpKind::TildeUnequal: { Return(TValue::Create<tBool>(UnsafeFloatUnequal(lhs, rhs))); }
    case BinOpKind::LeftShift: { break; }
    case BinOpKind::RightShift: { break; }
    case BinOpKind::BitwiseXor: { break; }
    default: { break; }     // not an arithmetic binary operator, shouldn't reach here
    }   /*switch*/
}

template<BinOpKind kind>
static void ALWAYS_INLINE DoIntegerArithOp(int32_t lhs, TValue rhs)
{
    if (likely(rhs.Is<tInt32>()))
    {
        DoIntegerIntegerArithOp<kind>(lhs, rhs.As<tInt32>());
    }
    else
    {
        // with the sole exception of the "equal" operator (lhs=integer case),
        // integer + non-number is undefined behavior in both SOM++ and TruffleSOM, just replicate their behavior
        //
        if constexpr(kind == BinOpKind::Equal)
        {
            if (!rhs.Is<tDouble>())
            {
                Return(TValue::Create<tBool>(false));
            }
        }
        DoIntegerDoubleArithOp<kind>(lhs, rhs.As<tDouble>());
    }
}

template<BinOpKind kind>
static void ALWAYS_INLINE DoDoubleArithOp(double lhs, TValue rhs)
{
    // The SOM behavior of all Arith operators in this file is to cast rhs to double if it's int32
    //
    if (likely(rhs.Is<tDouble>()))
    {
        DoDoubleArithOp<kind>(lhs, rhs.As<tDouble>());
    }
    else
    {
        // doublee + non-number is undefined behavior in both SOM++ and TruffleSOM, just replicate their behavior
        //
        DoDoubleArithOp<kind>(lhs, static_cast<double>(rhs.As<tInt32>()));
    }
}

template<BinOpKind kind>
static void NO_RETURN ArithBinOpImpl(TValue lhs, TValue rhs)
{
    if constexpr(kind == BinOpKind::And || kind == BinOpKind::LeftShift || kind == BinOpKind::RightShift || kind == BinOpKind::BitwiseXor)
    {
        // These operators are only defined on integer in the standard library, everything else must be handled by slow path
        //
        if (likely(lhs.Is<tInt32>()))
        {
            DoIntegerArithOp<kind>(lhs.As<tInt32>(), rhs);
        }
    }
    else if constexpr(kind == BinOpKind::Minus || kind == BinOpKind::Star || kind == BinOpKind::SlashSlash)
    {
        // For the above operators, double is more likely
        //
        if (likely(lhs.Is<tDouble>()))
        {
            DoDoubleArithOp<kind>(lhs.As<tDouble>(), rhs);
        }
        else if (likely(lhs.Is<tInt32>()))
        {
            DoIntegerArithOp<kind>(lhs.As<tInt32>(), rhs);
        }
    }
    else if constexpr(kind == BinOpKind::Equal)
    {
        // For equal, we need to additionally consider string equal which is a common case
        //
        if (likely(lhs.Is<tInt32>()))
        {
            DoIntegerArithOp<kind>(lhs.As<tInt32>(), rhs);
        }
        else if (likely(lhs.Is<tHeapEntity>()))
        {
            if (lhs.As<tHeapEntity>()->m_arrayType == SOM_String)
            {
                HeapPtr<SOMObject> l = lhs.As<tObject>();
                if (lhs.m_value == rhs.m_value) { Return(TValue::Create<tBool>(true)); }

                if (!rhs.Is<tHeapEntity>()) { Return(TValue::Create<tBool>(false)); }
                if (rhs.As<tHeapEntity>()->m_arrayType != SOM_String) { Return(TValue::Create<tBool>(false)); }
                HeapPtr<SOMObject> r = rhs.As<tObject>();

                size_t len = r->m_data[0].m_value;
                if (len != l->m_data[0].m_value) { Return(TValue::Create<tBool>(false)); }

                VM* vm = VM_GetActiveVMForCurrentThread();
                int res = memcmp(TranslateToRawPointer(vm, &l->m_data[1]), TranslateToRawPointer(vm, &r->m_data[1]), len);
                Return(TValue::Create<tBool>(res == 0));
            }
        }
        else if (likely(lhs.Is<tDouble>()))
        {
            DoDoubleArithOp<kind>(lhs.As<tDouble>(), rhs);
        }
    }
    else
    {
        // lhs is integer is more likely
        //
        if (likely(lhs.Is<tInt32>()))
        {
            DoIntegerArithOp<kind>(lhs.As<tInt32>(), rhs);
        }
        else if (likely(lhs.Is<tDouble>()))
        {
            DoDoubleArithOp<kind>(lhs.As<tDouble>(), rhs);
        }
    }
    // If we reach here, fast path has failed to handle the operation
    // Execute full slow path
    //
    EnterSlowPath<BinOpGeneralSlowPath<kind>>();
}

DEEGEN_DEFINE_BYTECODE_TEMPLATE(ArithBinOp, BinOpKind kind)
{
    Operands(
        BytecodeSlotOrConstant("lhs"),
        BytecodeSlotOrConstant("rhs")
    );
    Result(BytecodeValue);
    Implementation(ArithBinOpImpl<kind>);
    Variant(Op("lhs").IsBytecodeSlot(), Op("rhs").IsBytecodeSlot());
    Variant(Op("lhs").IsBytecodeSlot(), Op("rhs").IsConstant<tInt32>());
    Variant(Op("lhs").IsConstant<tInt32>(), Op("rhs").IsBytecodeSlot());
    if (kind != BinOpKind::And && kind != BinOpKind::LeftShift && kind != BinOpKind::RightShift && kind != BinOpKind::BitwiseXor)
    {
        Variant(Op("lhs").IsBytecodeSlot(), Op("rhs").IsConstant<tDouble>());
        Variant(Op("lhs").IsConstant<tDouble>(), Op("rhs").IsBytecodeSlot());
    }
    Variant(Op("lhs").IsConstant(), Op("rhs").IsConstant());
    if (kind == BinOpKind::Equal)
    {
        Variant(Op("lhs").IsBytecodeSlot(), Op("rhs").IsConstant());
        Variant(Op("lhs").IsConstant(), Op("rhs").IsBytecodeSlot());
    }
    DfgVariant();
    TypeDeductionRule(ValueProfile);
}

DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorPlus, ArithBinOp, BinOpKind::Plus);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorMinus, ArithBinOp, BinOpKind::Minus);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorStar, ArithBinOp, BinOpKind::Star);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorSlashSlash, ArithBinOp, BinOpKind::SlashSlash);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorPercent, ArithBinOp, BinOpKind::Percent);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorAnd, ArithBinOp, BinOpKind::And);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorEqual, ArithBinOp, BinOpKind::Equal);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorLessThan, ArithBinOp, BinOpKind::LessThan);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorLessEqual, ArithBinOp, BinOpKind::LessEqual);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorGreaterThan, ArithBinOp, BinOpKind::GreaterThan);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorGreaterEqual, ArithBinOp, BinOpKind::GreaterEqual);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorUnequal, ArithBinOp, BinOpKind::Unequal);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorTildeUnequal, ArithBinOp, BinOpKind::TildeUnequal);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorLeftShift, ArithBinOp, BinOpKind::LeftShift);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorRightShift, ArithBinOp, BinOpKind::RightShift);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorBitwiseXor, ArithBinOp, BinOpKind::BitwiseXor);

// operator == is always object equality and it's UB to specialize it
// (see ANSI Smalltalk standard on restrictive selectors)
// And yes, NaN == NaN, -0.0 != 0.0, this *is* SOM behavior
//
static void NO_RETURN OperatorEqualEqualImpl(TValue lhs, TValue rhs)
{
    Return(TValue::Create<tBool>(lhs.m_value == rhs.m_value));
}

DEEGEN_DEFINE_BYTECODE(OperatorEqualEqual)
{
    Operands(
        BytecodeSlotOrConstant("lhs"),
        BytecodeSlotOrConstant("rhs")
    );
    Result(BytecodeValue);
    Implementation(OperatorEqualEqualImpl);
    Variant(Op("lhs").IsBytecodeSlot(), Op("rhs").IsBytecodeSlot());
    Variant(Op("lhs").IsBytecodeSlot(), Op("rhs").IsConstant());
    Variant(Op("lhs").IsConstant(), Op("rhs").IsBytecodeSlot());
    Variant(Op("lhs").IsConstant(), Op("rhs").IsConstant());
    DfgVariant();
    TypeDeductionRule(AlwaysOutput<tBool>);
}

// Logical operators &&, ||, and:, or: are most likely working on boolean values
//
template<BinOpKind kind>
static void NO_RETURN BooleanBinOpImpl(TValue lhs, TValue rhs)
{
    // All non-heap-entity types (Nil/Boolean/Integer/Double)'s method "value" directly returns self (i.e., identity function)
    // For the other cases, we must invoke the "value" method. Let the slow path do that.
    //
    if (likely(lhs.Is<tBool>() && !rhs.Is<tHeapEntity>()))
    {
        if constexpr(kind == BinOpKind::LogicalAnd || kind == BinOpKind::KeywordAnd)
        {
            if (lhs.As<tBool>())
            {
                Return(rhs);
            }
            else
            {
                Return(lhs);
            }
        }
        else
        {
            static_assert(kind == BinOpKind::LogicalOr || kind == BinOpKind::KeywordOr);
            if (!lhs.As<tBool>())
            {
                Return(rhs);
            }
            else
            {
                Return(lhs);
            }
        }
    }
    // If we reach here, fast path has failed to handle the operation
    // Execute full slow path
    //
    EnterSlowPath<BinOpGeneralSlowPath<kind>>();
}

DEEGEN_DEFINE_BYTECODE_TEMPLATE(BooleanBinOp, BinOpKind kind)
{
    Operands(
        BytecodeSlot("lhs"),
        BytecodeSlot("rhs")
    );
    Result(BytecodeValue);
    Implementation(BooleanBinOpImpl<kind>);
    Variant();
    DfgVariant();
    TypeDeductionRule(ValueProfile);
}

DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorLogicalAnd, BooleanBinOp, BinOpKind::LogicalAnd);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorLogicalOr, BooleanBinOp, BinOpKind::LogicalOr);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorKeywordAnd, BooleanBinOp, BinOpKind::KeywordAnd);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorKeywordOr, BooleanBinOp, BinOpKind::KeywordOr);

static void NO_RETURN ArrayAtOutOfRangeSlowPath(TValue lhs, TValue rhs)
{
    HeapPtr<SOMObject> o = lhs.As<tObject>();
    int32_t idx = rhs.As<tInt32>();
    fprintf(stderr, "Array access out of bound: index = %d, size = %d\n",
        static_cast<int>(idx), static_cast<int>(o->m_data[0].m_value));
    abort();
}

static void NO_RETURN StringCharAtOutOfRangeSlowPath(TValue lhs, TValue rhs)
{
    HeapPtr<SOMObject> o = lhs.As<tObject>();
    int32_t idx = rhs.As<tInt32>();
    fprintf(stderr, "String charAt out of bound: index = %d, string length = %d\n",
            static_cast<int>(idx), static_cast<int>(o->m_data[0].m_value));
    abort();
}

static void NO_RETURN InitSingleCharStringSlowPath(TValue /*lhs*/, TValue /*rhs*/, uint8_t ch)
{
    TValue* resArr = VM::VM_GetCachedSingleCharStringArray();
    TestAssert(resArr[ch].m_value == 0);
    SOMObject* r = SOMObject::AllocateString(std::string_view(reinterpret_cast<char*>(&ch), 1));
    TValue tv = TValue::Create<tObject>(TranslateToHeapPtr(r));
    resArr[ch] = tv;
    Return(tv);
}

// "value:", "at:", "charAt:"
//
// These operators have a specific class that they are most commonly used,
// but can also be often used by normal objects, so we must provide fast path for both.
//
// "value:" most likely operate on block2
// "at:" most likely operate on array
// "charAt:" most likely operate on string
//
template<BinOpKind kind>
static void NO_RETURN MiscBinOpImpl(TValue lhs, TValue rhs)
{
    if (likely(lhs.Is<tHeapEntity>()))
    {
        if constexpr(kind == BinOpKind::ValueColon)
        {
            SOMDetailEntityType fnTy = static_cast<SOMDetailEntityType>(lhs.As<tHeapEntity>()->m_arrayType & 15);
            if (likely(fnTy == SOM_BlockOneArg || fnTy == SOM_BlockOneArgImmSelf))
            {
                TValue self;
                HeapPtr<FunctionObject> fn = lhs.As<tFunction>();
                if (fnTy == SOM_BlockOneArgImmSelf)
                {
                    self.m_value = fn->m_upvalues[0].m_value;
                }
                else
                {
                    self = *reinterpret_cast<Upvalue*>(fn->m_upvalues[0].m_value)->m_ptr;
                }
                MakeCall(fn, self, rhs, BinOpCallReturnContinuation);
            }
        }
        else if constexpr(kind == BinOpKind::AtColon)
        {
            if (likely(lhs.As<tHeapEntity>()->m_arrayType == SOM_Array))
            {
                int32_t idx = rhs.As<tInt32>();
                HeapPtr<SOMObject> o = lhs.As<tObject>();
                if (unlikely(static_cast<uint32_t>(idx) - 1 >= o->m_data[0].m_value))
                {
                    EnterSlowPath<ArrayAtOutOfRangeSlowPath>();
                }
                Return(TCGet(o->m_data[idx]));
            }
        }
        else
        {
            static_assert(kind == BinOpKind::CharAtColon);
            if (likely(lhs.As<tHeapEntity>()->m_arrayType == SOM_String))
            {
                int32_t idx = rhs.As<tInt32>();
                HeapPtr<SOMObject> o = lhs.As<tObject>();
                if (unlikely(static_cast<uint32_t>(idx) - 1 >= o->m_data[0].m_value))
                {
                    EnterSlowPath<StringCharAtOutOfRangeSlowPath>();
                }
                uint8_t ch = reinterpret_cast<HeapPtr<uint8_t>>(&o->m_data[1])[idx - 1];
                TValue* resArr = VM::VM_GetCachedSingleCharStringArray();
                if (likely(resArr[ch].m_value != 0))
                {
                    Return(resArr[ch]);
                }
                else
                {
                    EnterSlowPath<InitSingleCharStringSlowPath>(ch);
                }
            }
        }

        // When we reach here, the specialized fast path has failed
        // Run general SOM Call IC logic
        //
        SOMUniquedString meth = GetLookupKeyForBinaryOperator<kind>();

        HeapPtr<SOMObject> base = reinterpret_cast<HeapPtr<SOMObject>>(lhs.As<tHeapEntity>());
        ICHandler* ic = MakeInlineCache();
        ic->AddKey(base->m_hiddenClass).SpecifyImpossibleValue(0);
        ic->FuseICIntoInterpreterOpcode();
        auto [fnKind, fn] = ic->Body(
            [ic, base, meth]() -> std::pair<SOMMethodLookupResultKind, HeapPtr<FunctionObject>> {
                if (unlikely(base->m_type != HeapEntityType::Object))
                {
                    return std::make_pair(SOM_CallBaseNotObject, Undef<HeapPtr<FunctionObject>>());
                }
                HeapPtr<SOMClass> hc = SystemHeapPointer<SOMClass>(base->m_hiddenClass).As();
                GeneralHeapPointer<FunctionObject> f = SOMClass::GetMethod(hc, meth);
                if (f.m_value == 0)
                {
                    return std::make_pair(SOM_MethodNotFound, Undef<HeapPtr<FunctionObject>>());
                }
                uint8_t c_funcTy = f.As()->m_invalidArrayType >> 4;
                int32_t c_result = f.m_value;
                return ic->Effect([c_funcTy, c_result] {
                    IcSpecializeValueFullCoverage(c_funcTy, SOM_NormalMethod, SOM_LiteralReturn, SOM_GlobalReturn, SOM_Getter, SOM_Setter);
                    IcSpecifyCaptureValueRange(c_result, -2000000000, 0);
                    return std::make_pair(static_cast<SOMMethodLookupResultKind>(c_funcTy), GeneralHeapPointer<FunctionObject>(c_result).As());
                });
            });

        switch (fnKind)
        {
        case SOM_MethodNotFound:
        {
            EnterSlowPath<BinOpCallMethodNotFoundSlowPath>(meth);
        }
        case SOM_CallBaseNotObject:
        {
            EnterSlowPath<BinOpGeneralSlowPath<kind>>();
        }
        case SOM_NormalMethod:
        case SOM_LiteralReturn:     // TODO: provide specialized impls
        case SOM_GlobalReturn:
        case SOM_Getter:
        case SOM_Setter:
        {
            MakeCall(fn, lhs, rhs, BinOpCallReturnContinuation);
        }
        }   /*switch*/
    }
    EnterSlowPath<BinOpGeneralSlowPath<kind>>();
}

DEEGEN_DEFINE_BYTECODE_TEMPLATE(MiscBinOp, BinOpKind kind)
{
    Operands(
        BytecodeSlot("lhs"),
        BytecodeSlotOrConstant("rhs")
    );
    Result(BytecodeValue);
    Implementation(MiscBinOpImpl<kind>);
    Variant(Op("rhs").IsBytecodeSlot());
    Variant(Op("rhs").IsConstant());
    DfgVariant();
    TypeDeductionRule(ValueProfile);
}

DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorValueColon, MiscBinOp, BinOpKind::ValueColon);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorAtColon, MiscBinOp, BinOpKind::AtColon);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorCharAtColon, MiscBinOp, BinOpKind::CharAtColon);

DEEGEN_END_BYTECODE_DEFINITIONS
