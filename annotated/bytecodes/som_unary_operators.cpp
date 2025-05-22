#include "api_define_bytecode.h"
#include "deegen_api.h"
#include "api_inline_cache.h"

#include "runtime_utils.h"
#include "som_class.h"
#include "som_utils.h"
#include "som_call_utils.h"

// All specializable unary operators
//
enum class UnaryOpKind
{
    Abs,        // abs
    Sqrt,       // sqrt
    Value,      // value
    Not,        // not
    Length,     // length
};

// Below is the fallback generic slow path logic that correctly but slowly implements any unary operator
//
static void NO_RETURN UnaryOpCallReturnContinuation(TValue /*op*/)
{
    Return(GetReturnValue(0));
}

static void NO_RETURN UnaryOpCallMethodNotFoundSlowPath(TValue op, SOMUniquedString meth)
{
    VM* vm = VM_GetActiveVMForCurrentThread();
    HeapPtr<SOMClass> cl = GetSOMClassOfAny(op);
    GeneralHeapPointer<FunctionObject> handler = SOMClass::GetMethod(cl, vm->m_doesNotUnderstandHandler);
    TestAssert(handler.m_value != 0);
    TValue fnName = TValue::Create<tObject>(TranslateToHeapPtr(vm->GetInternedSymbol(meth.m_id)));
    SOMObject* args = SOMObject::AllocateArray(0 /*numArgs*/);
    MakeCall(handler.As(), op, fnName, TValue::Create<tObject>(TranslateToHeapPtr(args)), UnaryOpCallReturnContinuation);
}

template<UnaryOpKind kind>
static SOMUniquedString ALWAYS_INLINE WARN_UNUSED GetLookupKeyForUnaryOperator()
{
    VM* vm = VM_GetActiveVMForCurrentThread();
    switch (kind)
    {
    case UnaryOpKind::Abs: return vm->m_strOperatorAbs;
    case UnaryOpKind::Sqrt: return vm->m_strOperatorSqrt;
    case UnaryOpKind::Value: return vm->m_strOperatorValue;
    case UnaryOpKind::Not: return vm->m_strOperatorNot;
    case UnaryOpKind::Length: return vm->m_strOperatorLength;
    }   /*switch*/
    __builtin_unreachable();
}

template<UnaryOpKind kind>
static void NO_RETURN UnaryOpGeneralSlowPath(TValue op)
{
    HeapPtr<SOMClass> cl = GetSOMClassOfAny(op);
    SOMUniquedString meth = GetLookupKeyForUnaryOperator<kind>();
    GeneralHeapPointer<FunctionObject> f = SOMClass::GetMethod(cl, meth);
    if (f.m_value == 0)
    {
        EnterSlowPath<UnaryOpCallMethodNotFoundSlowPath>(meth);
    }
    MakeCall(f.As(), op, UnaryOpCallReturnContinuation);
}

template<UnaryOpKind kind>
static void NO_RETURN ArithUnaryOpImpl(TValue op)
{
    if constexpr(kind == UnaryOpKind::Abs)
    {
        if (likely(op.Is<tInt32>()))
        {
            int32_t v = op.As<tInt32>();
            if (v < 0) { v = -v; }
            Return(TValue::Create<tInt32>(v));
        }
        else if (likely(op.Is<tDouble>()))
        {
            double d = op.As<tDouble>();
            if (d < 0.0)
            {
                d = 0.0 - d;
            }
            Return(TValue::Create<tDouble>(d));
        }
    }
    else
    {
        static_assert(kind == UnaryOpKind::Sqrt);
        if (likely(op.Is<tDouble>()))
        {
            double d = op.As<tDouble>();
            d = __builtin_sqrt(d);
            Return(TValue::Create<tDouble>(d));
        }
        else if (likely(op.Is<tInt32>()))
        {
            int32_t v = op.As<tInt32>();
            double r = __builtin_sqrt(static_cast<double>(v));
            if (r == rint(r))
            {
                Return(TValue::Create<tInt32>(static_cast<int32_t>(r)));
            }
            else
            {
                Return(TValue::Create<tDouble>(r));
            }
        }
    }

    EnterSlowPath<UnaryOpGeneralSlowPath<kind>>();
}

DEEGEN_DEFINE_BYTECODE_TEMPLATE(ArithUnaryOp, UnaryOpKind kind)
{
    Operands(
        BytecodeSlot("op")
    );
    Result(BytecodeValue);
    Implementation(ArithUnaryOpImpl<kind>);
    Variant();
    DfgVariant();
    TypeDeductionRule(ValueProfile);
}

DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorAbs, ArithUnaryOp, UnaryOpKind::Abs);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorSqrt, ArithUnaryOp, UnaryOpKind::Sqrt);

// SOM++ already did unsound inlining of ifNil:ifNotNil, so it's already assuming that ifNil cannot be overloaded by users
// This is not strictly what is allowed by ANSI Smalltalk standard (ANSI Smalltalk standard only says ifTrue:ifFalse cannot
// be overloaded), but since SOM++ is already doing this, let's do the same..
//
template<bool forIsNil>
static void NO_RETURN IsNilNotNilOpImpl(TValue op)
{
    Return(TValue::Create<tBool>(op.Is<tNil>() == forIsNil));
}

DEEGEN_DEFINE_BYTECODE_TEMPLATE(IsNilNotNilOp, bool forIsNil)
{
    Operands(
        BytecodeSlot("op")
    );
    Result(BytecodeValue);
    Implementation(IsNilNotNilOpImpl<forIsNil>);
    Variant();
    DfgVariant();
    TypeDeductionRule(AlwaysOutput<tBool>);
}

DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorIsNil, IsNilNotNilOp, true /*forIsNil*/);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorNotNil, IsNilNotNilOp, false /*forIsNil*/);

template<UnaryOpKind kind>
static void NO_RETURN MiscUnaryOpImpl(TValue op)
{
    if constexpr(kind == UnaryOpKind::Not)
    {
        if (likely(op.Is<tBool>()))
        {
            Return(TValue::Create<tBool>(!op.As<tBool>()));
        }
    }
    if (likely(op.Is<tHeapEntity>()))
    {
        if constexpr(kind == UnaryOpKind::Value)
        {
            SOMDetailEntityType fnTy = static_cast<SOMDetailEntityType>(op.As<tHeapEntity>()->m_arrayType & 15);
            if (likely(fnTy == SOM_BlockNoArg || fnTy == SOM_BlockNoArgImmSelf))
            {
                TValue self;
                HeapPtr<FunctionObject> fn = op.As<tFunction>();
                if (fnTy == SOM_BlockNoArgImmSelf)
                {
                    self.m_value = fn->m_upvalues[0].m_value;
                }
                else
                {
                    self = *reinterpret_cast<Upvalue*>(fn->m_upvalues[0].m_value)->m_ptr;
                }
                MakeCall(fn, self, UnaryOpCallReturnContinuation);
            }
        }
        else if constexpr(kind == UnaryOpKind::Length)
        {
            uint8_t ty = op.As<tHeapEntity>()->m_arrayType;
            if (likely(ty == SOM_Array || ty == SOM_String))
            {
                Return(TValue::Create<tInt32>(static_cast<int32_t>(op.As<tObject>()->m_data[0].m_value)));
            }
        }
        else
        {
            static_assert(kind == UnaryOpKind::Not);
        }

        // When we reach here, the specialized fast path has failed
        // Run general SOM Call IC logic
        //
        SOMUniquedString meth = GetLookupKeyForUnaryOperator<kind>();

        HeapPtr<SOMObject> base = reinterpret_cast<HeapPtr<SOMObject>>(op.As<tHeapEntity>());
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
                if (c_funcTy == SOM_GlobalReturn)
                {
                    Assert(f.As()->m_numUpvalues == 1);
                    TValue r = VM::VM_GetGlobal(f.As()->m_upvalues[0].m_value);
                    if (r.m_value == TValue::CreateImpossibleValue().m_value)
                    {
                        return std::make_pair(SOM_NormalMethod, f.As());
                    }
                }
                if (c_funcTy == SOM_GlobalReturn || c_funcTy == SOM_Getter || c_funcTy == SOM_Setter)
                {
                    Assert(f.As()->m_numUpvalues == 1);
                    int32_t c_result = SafeIntegerCast<int32_t>(f.As()->m_upvalues[0].m_value);
                    return ic->Effect([c_funcTy, c_result] {
                        IcSpecializeValueFullCoverage(c_funcTy, SOM_GlobalReturn, SOM_Getter, SOM_Setter);
                        IcSpecifyCaptureValueRange(c_result, 0, 100000000);
                        return std::make_pair(static_cast<SOMMethodLookupResultKind>(c_funcTy), reinterpret_cast<HeapPtr<FunctionObject>>(static_cast<uint64_t>(c_result)));
                    });
                }
                else
                {
                    int32_t c_result = f.m_value;
                    return ic->Effect([c_funcTy, c_result] {
                        IcSpecializeValueFullCoverage(c_funcTy, SOM_NormalMethod, SOM_LiteralReturn, SOM_SelfReturn);
                        IcSpecifyCaptureValueRange(c_result, -2000000000, 0);
                        return std::make_pair(static_cast<SOMMethodLookupResultKind>(c_funcTy), GeneralHeapPointer<FunctionObject>(c_result).As());
                    });
                }
            });

        switch (fnKind)
        {
        case SOM_MethodNotFound:
        {
            EnterSlowPath<UnaryOpCallMethodNotFoundSlowPath>(meth);
        }
        case SOM_CallBaseNotObject:
        {
            EnterSlowPath<UnaryOpGeneralSlowPath<kind>>();
        }
        case SOM_NormalMethod:
        {
            MakeCall(fn, op, UnaryOpCallReturnContinuation);
        }
        case SOM_Setter:
        {
            // Ugly: this really should be __builtin_unreachable(),
            // but LLVM will generate a jump table for this switch, and if an entry is unreachable,
            // LLVM will not fill nullptr to that entry, but fill an arbitrary nonsense asm label!
            // This is fine for LLVM because we've promised LLVM the entry is unreachable anyway,
            // but our ASM analyzer does not know that, and will be confused by the nonsense label
            // and trigger assertions... So, use __builtin_trap() to force some useful logic emitted.
            //
            TestAssert(false);
            __builtin_trap();
        }
        default:
        {
            Return(ExecuteTrivialMethodExceptSetter(fn, op, fnKind));
        }
        }   /*switch*/
    }
    if constexpr(kind == UnaryOpKind::Value)
    {
        // 'value' is defined as identity function on all non-heap-entity values
        //
        Return(op);
    }
    EnterSlowPath<UnaryOpGeneralSlowPath<kind>>();
}

DEEGEN_DEFINE_BYTECODE_TEMPLATE(MiscUnaryOp, UnaryOpKind kind)
{
    Operands(
        BytecodeSlot("op")
    );
    Result(BytecodeValue);
    Implementation(MiscUnaryOpImpl<kind>);
    Variant();
    DfgVariant();
    TypeDeductionRule(ValueProfile);
}

DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorValue, MiscUnaryOp, UnaryOpKind::Value);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorNot, MiscUnaryOp, UnaryOpKind::Not);
DEEGEN_DEFINE_BYTECODE_BY_TEMPLATE_INSTANTIATION(OperatorLength, MiscUnaryOp, UnaryOpKind::Length);

DEEGEN_END_BYTECODE_DEFINITIONS
