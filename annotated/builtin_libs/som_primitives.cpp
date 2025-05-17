#include "deegen_api.h"
#include "runtime_utils.h"
#include "som_class.h"
#include "som_utils.h"
#include "som_compile_file.h"
#include <sstream>
#include <fstream>
#include "vm.h"

#ifdef ENABLE_SOM_PROFILE_FREQUENCY
#define SOM_LOG_PRIMITIVE_FREQ(name) do { VM_GetActiveVMForCurrentThread()->IncrementPrimitiveFuncCallCount(PP_STRINGIFY(name)); } while (false)
#else
#define SOM_LOG_PRIMITIVE_FREQ(name) do { } while (false)
#endif

void NO_INLINE PrintSOMStackTrace(FILE* file, StackFrameHeader* hdr)
{
    VM* vm = VM_GetActiveVMForCurrentThread();
    while (true)
    {
        FunctionObject* func = TranslateToRawPointer(vm, hdr->m_func);
        ExecutableCode* ec = TranslateToRawPointer(vm, func->m_executable.As());
        if (ec->IsUserCFunction())
        {
            fprintf(file, "%s\n", vm->m_somPrimitives.LookupFunctionObject(func).c_str());
        }
        else
        {
            UnlinkedCodeBlock* ucb = static_cast<CodeBlock*>(ec)->m_owner;
            if (ucb->m_parent != nullptr)
            {
                fprintf(file, "block@");
                while (ucb->m_parent != nullptr)
                {
                    ucb = ucb->m_parent;
                }
            }
            TestAssert(ucb->m_debugName != "");
            fprintf(file, "%s\n", ucb->m_debugName.c_str());
        }
        if (hdr->m_caller == nullptr) { break; }
        hdr = reinterpret_cast<StackFrameHeader*>(hdr->m_caller) - 1;
    }
}

// SOM is not a safe language, there is no specification on what happens if rhs is not a number,
// and the behaviors of the reference implementations are simply UB (SOM++) or crash (TruffleSOM)
// so we do the same..
//
DEEGEN_DEFINE_LIB_FUNC(integer_add)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_add);

    int32_t lhs = GetArg(0).As<tInt32>();
    TValue rhs = GetArg(1);
    if (rhs.Is<tInt32>())
    {
        Return(TValue::Create<tInt32>(lhs + rhs.As<tInt32>()));
    }
    else
    {
        Assert(rhs.Is<tDouble>());
        Return(TValue::Create<tDouble>(lhs + rhs.As<tDouble>()));
    }
}

DEEGEN_DEFINE_LIB_FUNC(integer_minus)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_minus);

    int32_t lhs = GetArg(0).As<tInt32>();
    TValue rhs = GetArg(1);
    if (rhs.Is<tInt32>())
    {
        Return(TValue::Create<tInt32>(lhs - rhs.As<tInt32>()));
    }
    else
    {
        Assert(rhs.Is<tDouble>());
        Return(TValue::Create<tDouble>(lhs - rhs.As<tDouble>()));
    }
}

DEEGEN_DEFINE_LIB_FUNC(integer_star)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_star);

    int32_t lhs = GetArg(0).As<tInt32>();
    TValue rhs = GetArg(1);
    if (rhs.Is<tInt32>())
    {
        Return(TValue::Create<tInt32>(lhs * rhs.As<tInt32>()));
    }
    else
    {
        Assert(rhs.Is<tDouble>());
        Return(TValue::Create<tDouble>(lhs * rhs.As<tDouble>()));
    }
}

DEEGEN_DEFINE_LIB_FUNC(integer_rem)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_rem);

    int32_t l = GetArg(0).As<tInt32>();
    TValue rhs = GetArg(1);
    if (rhs.Is<tInt32>())
    {
        int32_t r = rhs.As<tInt32>();
        Return(TValue::Create<tInt32>(l - ((l / r) * r)));
    }
    else
    {
        Assert(rhs.Is<tDouble>());
        Return(TValue::Create<tDouble>(static_cast<double>(l % static_cast<int64_t>(rhs.As<tDouble>()))));
    }
}

DEEGEN_DEFINE_LIB_FUNC(integer_bitwisexor)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_bitwisexor);

    int32_t lhs = GetArg(0).As<tInt32>();
    int32_t rhs = GetArg(1).As<tInt32>();
    Return(TValue::Create<tInt32>(lhs ^ rhs));
}

DEEGEN_DEFINE_LIB_FUNC(integer_leftshift)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_leftshift);

    int32_t lhs = GetArg(0).As<tInt32>();
    int32_t rhs = GetArg(1).As<tInt32>();
    Return(TValue::Create<tInt32>(lhs << rhs));
}

DEEGEN_DEFINE_LIB_FUNC(integer_unsignedrightshift)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_unsignedrightshift);

    int32_t lhs = GetArg(0).As<tInt32>();
    int32_t rhs = GetArg(1).As<tInt32>();
    Return(TValue::Create<tInt32>(lhs >> rhs));
}

DEEGEN_DEFINE_LIB_FUNC(integer_slash)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_slash);

    int32_t l = GetArg(0).As<tInt32>();
    TValue rhs = GetArg(1);
    if (rhs.Is<tInt32>())
    {
        int32_t r = rhs.As<tInt32>();
        Return(TValue::Create<tInt32>(l / r));
    }
    else
    {
        Assert(rhs.Is<tDouble>());
        Return(TValue::Create<tInt32>(l / static_cast<int32_t>(rhs.As<tDouble>())));
    }
}

DEEGEN_DEFINE_LIB_FUNC(integer_slashslash)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_slashslash);

    int32_t l = GetArg(0).As<tInt32>();
    TValue rhs = GetArg(1);
    if (rhs.Is<tInt32>())
    {
        int32_t r = rhs.As<tInt32>();
        Return(TValue::Create<tDouble>(static_cast<double>(l) / r));
    }
    else
    {
        Assert(rhs.Is<tDouble>());
        Return(TValue::Create<tDouble>(l / rhs.As<tDouble>()));
    }
}

DEEGEN_DEFINE_LIB_FUNC(integer_percent)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_percent);

    int32_t l = GetArg(0).As<tInt32>();
    TValue rhs = GetArg(1);
    int32_t r;
    if (rhs.Is<tInt32>())
    {
        r = rhs.As<tInt32>();
    }
    else
    {
        Assert(rhs.Is<tDouble>());
        r = static_cast<int32_t>(rhs.As<tDouble>());
    }
    int32_t result = l % r;
    if ((result != 0) && ((result < 0) != (r < 0))) {
        result += r;
    }
    Return(TValue::Create<tInt32>(result));
}

DEEGEN_DEFINE_LIB_FUNC(integer_and)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_and);

    int32_t lhs = GetArg(0).As<tInt32>();
    int32_t rhs = GetArg(1).As<tInt32>();
    Return(TValue::Create<tInt32>(lhs & rhs));
}

DEEGEN_DEFINE_LIB_FUNC(integer_equal)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_equal);

    int32_t l = GetArg(0).As<tInt32>();
    TValue rhs = GetArg(1);
    if (rhs.Is<tInt32>())
    {
        Return(TValue::Create<tBool>(l == rhs.As<tInt32>()));
    }
    else if (rhs.Is<tDouble>())
    {
        Return(TValue::Create<tBool>(UnsafeFloatEqual(static_cast<double>(l), rhs.As<tDouble>())));
    }
    else
    {
        Return(TValue::Create<tBool>(false));
    }
}

DEEGEN_DEFINE_LIB_FUNC(integer_equalequal)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_equalequal);

    int32_t l = GetArg(0).As<tInt32>();
    TValue rhs = GetArg(1);
    if (rhs.Is<tInt32>())
    {
        Return(TValue::Create<tBool>(l == rhs.As<tInt32>()));
    }
    else
    {
        Return(TValue::Create<tBool>(false));
    }
}

DEEGEN_DEFINE_LIB_FUNC(integer_lowerthan)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_lowerthan);

    int32_t lhs = GetArg(0).As<tInt32>();
    TValue rhs = GetArg(1);
    if (rhs.Is<tInt32>())
    {
        Return(TValue::Create<tBool>(lhs < rhs.As<tInt32>()));
    }
    else
    {
        Assert(rhs.Is<tDouble>());
        Return(TValue::Create<tBool>(lhs < rhs.As<tDouble>()));
    }
}

DEEGEN_DEFINE_LIB_FUNC(integer_lowerequal)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_lowerequal);

    int32_t lhs = GetArg(0).As<tInt32>();
    TValue rhs = GetArg(1);
    if (rhs.Is<tInt32>())
    {
        Return(TValue::Create<tBool>(lhs <= rhs.As<tInt32>()));
    }
    else
    {
        Assert(rhs.Is<tDouble>());
        Return(TValue::Create<tBool>(lhs <= rhs.As<tDouble>()));
    }
}

DEEGEN_DEFINE_LIB_FUNC(integer_greaterthan)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_greaterthan);

    int32_t lhs = GetArg(0).As<tInt32>();
    TValue rhs = GetArg(1);
    if (rhs.Is<tInt32>())
    {
        Return(TValue::Create<tBool>(lhs > rhs.As<tInt32>()));
    }
    else
    {
        Assert(rhs.Is<tDouble>());
        Return(TValue::Create<tBool>(lhs > rhs.As<tDouble>()));
    }
}

DEEGEN_DEFINE_LIB_FUNC(integer_greaterequal)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_greaterequal);

    int32_t lhs = GetArg(0).As<tInt32>();
    TValue rhs = GetArg(1);
    if (rhs.Is<tInt32>())
    {
        Return(TValue::Create<tBool>(lhs >= rhs.As<tInt32>()));
    }
    else
    {
        Assert(rhs.Is<tDouble>());
        Return(TValue::Create<tBool>(lhs >= rhs.As<tDouble>()));
    }
}

DEEGEN_DEFINE_LIB_FUNC(integer_unequal)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_unequal);

    int32_t lhs = GetArg(0).As<tInt32>();
    TValue rhs = GetArg(1);
    if (rhs.Is<tInt32>())
    {
        Return(TValue::Create<tBool>(lhs != rhs.As<tInt32>()));
    }
    else
    {
        Assert(rhs.Is<tDouble>());
        Return(TValue::Create<tBool>(UnsafeFloatUnequal(static_cast<double>(lhs), rhs.As<tDouble>())));
    }
}

DEEGEN_DEFINE_LIB_FUNC(integer_asstring)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_asstring);

    int32_t v = GetArg(0).As<tInt32>();
    std::ostringstream Str;
    Str << v;
    SOMObject* s = SOMObject::AllocateString(Str.str());
    Return(TValue::Create<tObject>(TranslateToHeapPtr(s)));
}

DEEGEN_DEFINE_LIB_FUNC(integer_asdouble)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_asdouble);

    int32_t v = GetArg(0).As<tInt32>();
    Return(TValue::Create<tDouble>(static_cast<double>(v)));
}

DEEGEN_DEFINE_LIB_FUNC(integer_as32bitsigned)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_as32bitsigned);

    Return(GetArg(0));
}

DEEGEN_DEFINE_LIB_FUNC(integer_as32bitunsigned)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_as32bitunsigned);

    Return(GetArg(0));
}

DEEGEN_DEFINE_LIB_FUNC(integer_sqrt)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_sqrt);

    int32_t v = GetArg(0).As<tInt32>();
    double r = sqrt(static_cast<double>(v));
    if (r == rint(r))
    {
        Return(TValue::Create<tInt32>(static_cast<int32_t>(r)));
    }
    else
    {
        Return(TValue::Create<tDouble>(r));
    }
}

DEEGEN_DEFINE_LIB_FUNC(integer_atrandom)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_atrandom);

    Return(TValue::Create<tInt32>(rand()));
}

DEEGEN_DEFINE_LIB_FUNC(integer_fromstring)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_fromstring);

    TValue tv = GetArg(1);
    TestAssert(tv.Is<tObject>());
    HeapPtr<SOMObject> o = tv.As<tObject>();
    TestAssert(o->m_arrayType == SOM_String);
    char* str = reinterpret_cast<char*>(TranslateToRawPointer(&o->m_data[1]));

    errno = 0;

    char* pEnd{};

    const int64_t i = std::strtoll(str, &pEnd, 10);

    if (str == pEnd) {
        // did not parse anything
        Return(TValue::Create<tInt32>(0));
    }

    const bool rangeError = errno == ERANGE;
    if (rangeError) {
        // TODO(smarr): try a big int library
        Return(TValue::Create<tInt32>(0));
    }

    Return(TValue::Create<tInt32>(static_cast<int32_t>(i)));
}

DEEGEN_DEFINE_LIB_FUNC(integer_abs)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_abs);

    int32_t v = GetArg(0).As<tInt32>();
    if (v < 0) { v = -v; }
    Return(TValue::Create<tInt32>(v));
}

DEEGEN_DEFINE_LIB_FUNC(integer_min)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_min);

    int32_t v = GetArg(0).As<tInt32>();
    TValue rhs = GetArg(1);
    if (rhs.Is<tInt32>())
    {
        Return(TValue::Create<tInt32>(std::min(v, rhs.As<tInt32>())));
    }
    else
    {
        Assert(rhs.Is<tDouble>());
        double r = rhs.As<tDouble>();
        if (v < r)
        {
            Return(GetArg(0));
        }
        else
        {
            Return(GetArg(1));
        }
    }
}

DEEGEN_DEFINE_LIB_FUNC(integer_max)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_max);

    int32_t v = GetArg(0).As<tInt32>();
    TValue rhs = GetArg(1);
    if (rhs.Is<tInt32>())
    {
        Return(TValue::Create<tInt32>(std::max(v, rhs.As<tInt32>())));
    }
    else
    {
        Assert(rhs.Is<tDouble>());
        double r = rhs.As<tDouble>();
        if (v > r)
        {
            Return(GetArg(0));
        }
        else
        {
            Return(GetArg(1));
        }
    }
}

DEEGEN_DEFINE_LIB_FUNC(integer_range)
{
    SOM_LOG_PRIMITIVE_FREQ(integer_range);

    int32_t start = GetArg(0).As<tInt32>();
    int32_t end = GetArg(1).As<tInt32>();
    size_t len = (end >= start) ? static_cast<size_t>(end - start + 1) : 0;
    SOMObject* o = SOMObject::AllocateArray(len);
    for (int32_t curv = start; curv <= end; curv++)
    {
        o->m_data[curv - start + 1] = TValue::Create<tInt32>(curv);
    }
    Return(TValue::Create<tObject>(TranslateToHeapPtr(o)));
}

static double ALWAYS_INLINE CoerceDouble(TValue tv)
{
    if (tv.Is<tDouble>()) { return tv.As<tDouble>(); }
    Assert(tv.Is<tInt32>());
    return static_cast<double>(tv.As<tInt32>());
}

DEEGEN_DEFINE_LIB_FUNC(double_add)
{
    SOM_LOG_PRIMITIVE_FREQ(double_add);

    double lhs = GetArg(0).As<tDouble>();
    double rhs = CoerceDouble(GetArg(1));
    Return(TValue::Create<tDouble>(lhs + rhs));
}

DEEGEN_DEFINE_LIB_FUNC(double_sub)
{
    SOM_LOG_PRIMITIVE_FREQ(double_sub);

    double lhs = GetArg(0).As<tDouble>();
    double rhs = CoerceDouble(GetArg(1));
    Return(TValue::Create<tDouble>(lhs - rhs));
}

DEEGEN_DEFINE_LIB_FUNC(double_star)
{
    SOM_LOG_PRIMITIVE_FREQ(double_star);

    double lhs = GetArg(0).As<tDouble>();
    double rhs = CoerceDouble(GetArg(1));
    Return(TValue::Create<tDouble>(lhs * rhs));
}

DEEGEN_DEFINE_LIB_FUNC(double_slashslash)
{
    SOM_LOG_PRIMITIVE_FREQ(double_slashslash);

    double lhs = GetArg(0).As<tDouble>();
    double rhs = CoerceDouble(GetArg(1));
    Return(TValue::Create<tDouble>(lhs / rhs));
}

DEEGEN_DEFINE_LIB_FUNC(double_percent)
{
    SOM_LOG_PRIMITIVE_FREQ(double_percent);

    double lhs = GetArg(0).As<tDouble>();
    double rhs = CoerceDouble(GetArg(1));
    Return(TValue::Create<tDouble>(static_cast<double>(static_cast<int64_t>(lhs) % static_cast<int64_t>(rhs))));
}

DEEGEN_DEFINE_LIB_FUNC(double_sin)
{
    SOM_LOG_PRIMITIVE_FREQ(double_sin);

    double v = GetArg(0).As<tDouble>();
    Return(TValue::Create<tDouble>(sin(v)));
}

DEEGEN_DEFINE_LIB_FUNC(double_cos)
{
    SOM_LOG_PRIMITIVE_FREQ(double_cos);

    double v = GetArg(0).As<tDouble>();
    Return(TValue::Create<tDouble>(cos(v)));
}

DEEGEN_DEFINE_LIB_FUNC(double_equal)
{
    SOM_LOG_PRIMITIVE_FREQ(double_equal);

    double lhs = GetArg(0).As<tDouble>();
    double rhs = CoerceDouble(GetArg(1));
    Return(TValue::Create<tBool>(UnsafeFloatEqual(lhs, rhs)));
}

DEEGEN_DEFINE_LIB_FUNC(double_unequal)
{
    SOM_LOG_PRIMITIVE_FREQ(double_unequal);

    double lhs = GetArg(0).As<tDouble>();
    double rhs = CoerceDouble(GetArg(1));
    Return(TValue::Create<tBool>(UnsafeFloatUnequal(lhs, rhs)));
}

DEEGEN_DEFINE_LIB_FUNC(double_lowerthan)
{
    SOM_LOG_PRIMITIVE_FREQ(double_lowerthan);

    double lhs = GetArg(0).As<tDouble>();
    double rhs = CoerceDouble(GetArg(1));
    Return(TValue::Create<tBool>(lhs < rhs));
}

DEEGEN_DEFINE_LIB_FUNC(double_lowerequal)
{
    SOM_LOG_PRIMITIVE_FREQ(double_lowerequal);

    double lhs = GetArg(0).As<tDouble>();
    double rhs = CoerceDouble(GetArg(1));
    Return(TValue::Create<tBool>(lhs <= rhs));
}

DEEGEN_DEFINE_LIB_FUNC(double_greaterthan)
{
    SOM_LOG_PRIMITIVE_FREQ(double_greaterthan);

    double lhs = GetArg(0).As<tDouble>();
    double rhs = CoerceDouble(GetArg(1));
    Return(TValue::Create<tBool>(lhs > rhs));
}

DEEGEN_DEFINE_LIB_FUNC(double_greaterequal)
{
    SOM_LOG_PRIMITIVE_FREQ(double_greaterequal);

    double lhs = GetArg(0).As<tDouble>();
    double rhs = CoerceDouble(GetArg(1));
    Return(TValue::Create<tBool>(lhs >= rhs));
}

DEEGEN_DEFINE_LIB_FUNC(double_min)
{
    SOM_LOG_PRIMITIVE_FREQ(double_min);

    double lhs = GetArg(0).As<tDouble>();
    double rhs = CoerceDouble(GetArg(1));
    if (lhs < rhs)
    {
        Return(GetArg(0));
    }
    else
    {
        Return(GetArg(1));
    }
}

DEEGEN_DEFINE_LIB_FUNC(double_max)
{
    SOM_LOG_PRIMITIVE_FREQ(double_max);

    double lhs = GetArg(0).As<tDouble>();
    double rhs = CoerceDouble(GetArg(1));
    if (lhs > rhs)
    {
        Return(GetArg(0));
    }
    else
    {
        Return(GetArg(1));
    }
}

DEEGEN_DEFINE_LIB_FUNC(double_asstring)
{
    SOM_LOG_PRIMITIVE_FREQ(double_asstring);

    double v = GetArg(0).As<tDouble>();
    std::ostringstream Str;
    Str.precision(17);
    Str << v;
    SOMObject* s = SOMObject::AllocateString(Str.str());
    Return(TValue::Create<tObject>(TranslateToHeapPtr(s)));
}

DEEGEN_DEFINE_LIB_FUNC(double_asinteger)
{
    SOM_LOG_PRIMITIVE_FREQ(double_asinteger);

    double v = GetArg(0).As<tDouble>();
    Return(TValue::Create<tInt32>(static_cast<int32_t>(v)));
}

DEEGEN_DEFINE_LIB_FUNC(double_round)
{
    SOM_LOG_PRIMITIVE_FREQ(double_round);

    double v = GetArg(0).As<tDouble>();
    Return(TValue::Create<tInt32>(static_cast<int32_t>(llround(v))));
}

DEEGEN_DEFINE_LIB_FUNC(double_sqrt)
{
    SOM_LOG_PRIMITIVE_FREQ(double_sqrt);

    double v = GetArg(0).As<tDouble>();
    Return(TValue::Create<tDouble>(sqrt(v)));
}

DEEGEN_DEFINE_LIB_FUNC(double_positiveinfinity)
{
    SOM_LOG_PRIMITIVE_FREQ(double_positiveinfinity);

    Return(TValue::Create<tDouble>(std::numeric_limits<double>::infinity()));
}

DEEGEN_DEFINE_LIB_FUNC(double_fromstring)
{
    SOM_LOG_PRIMITIVE_FREQ(double_fromstring);

    TValue tv = GetArg(1);
    TestAssert(tv.Is<tObject>());
    HeapPtr<SOMObject> o = tv.As<tObject>();
    TestAssert(o->m_arrayType == SOM_String);
    char* str = reinterpret_cast<char*>(TranslateToRawPointer(&o->m_data[1]));
    double const value = stod(std::string(str, o->m_data[0].m_value));
    Return(TValue::Create<tDouble>(value));
}

DEEGEN_DEFINE_LIB_FUNC(object_equalequal)
{
    SOM_LOG_PRIMITIVE_FREQ(object_equalequal);

    TValue lhs = GetArg(0);
    TValue rhs = GetArg(1);
    Return(TValue::Create<tBool>(lhs.m_value == rhs.m_value));
}

DEEGEN_DEFINE_LIB_FUNC(object_sizebytes)
{
    SOM_LOG_PRIMITIVE_FREQ(object_sizebytes);

    TValue tv = GetArg(0);
    if (tv.Is<tObject>())
    {
        HeapPtr<SOMObject> o = tv.As<tObject>();
        if (o->m_arrayType == SOM_Array)
        {
            Return(TValue::Create<tInt32>(static_cast<int32_t>(o->m_data[0].m_value * 8 + 16)));
        }
        else if (o->m_arrayType == SOM_String)
        {
            size_t lenInclZero = o->m_data[0].m_value + 1;
            Return(TValue::Create<tInt32>(static_cast<int32_t>((lenInclZero + 7) / 8 * 8 + 16)));
        }
        else
        {
            HeapPtr<SOMClass> c = SystemHeapPointer<SOMClass>(o->m_hiddenClass).As();
            Return(TValue::Create<tInt32>(static_cast<int32_t>(c->m_numFields * 8 + 8)));
        }
    }
    else if (tv.Is<tFunction>())
    {
        HeapPtr<FunctionObject> o = tv.As<tFunction>();
        Return(TValue::Create<tInt32>(static_cast<int32_t>(o->m_numUpvalues * 8 + 8)));
    }
    else
    {
        Return(TValue::Create<tInt32>(8));
    }
}

// SOM++'s implementation use address/plain value for everything (except string, which overloads the hashcode)
//
DEEGEN_DEFINE_LIB_FUNC(object_hashcode)
{
    SOM_LOG_PRIMITIVE_FREQ(object_hashcode);

    TValue tv = GetArg(0);
    Return(TValue::Create<tInt32>(static_cast<int32_t>(HashPrimitiveTypes(tv.m_value))));
}

DEEGEN_DEFINE_LIB_FUNC(object_inspect)
{
    SOM_LOG_PRIMITIVE_FREQ(object_inspect);

    Return(TValue::Create<tBool>(false));
}

DEEGEN_DEFINE_LIB_FUNC(object_halt)
{
    SOM_LOG_PRIMITIVE_FREQ(object_halt);

    Return(TValue::Create<tBool>(false));
}

SOMUniquedString ALWAYS_INLINE GetUniquedStringFromVM(VM* vm, TValue meth)
{
    TestAssert(meth.Is<tObject>());
    TestAssert(meth.As<tObject>()->m_arrayType == SOM_String);
    HeapPtr<SOMObject> o = meth.As<tObject>();
    char* str = reinterpret_cast<char*>(TranslateToRawPointer(vm, &o->m_data[1]));
    size_t len = o->m_data[0].m_value;
    size_t ord = vm->m_interner.InternString(std::string_view(str, len));
    uint64_t hash = vm->m_interner.GetHash(ord);
    return SOMUniquedString {
        .m_id = static_cast<uint32_t>(ord),
        .m_hash = static_cast<uint32_t>(hash)
    };
}

DEEGEN_DEFINE_LIB_FUNC_CONTINUATION(TrivialReturnCont)
{
    Return(GetReturnValuesBegin()[0]);
}

DEEGEN_DEFINE_LIB_FUNC(object_perform)
{
    SOM_LOG_PRIMITIVE_FREQ(object_perform);

    TValue* base = GetStackBase();
    TValue self = GetArg(0);
    TValue meth = GetArg(1);
    VM* vm = VM_GetActiveVMForCurrentThread();
    SOMUniquedString methStr = GetUniquedStringFromVM(vm, meth);
    HeapPtr<SOMClass> cl = GetSOMClassOfAny(self);
    GeneralHeapPointer<FunctionObject> fn = SOMClass::GetMethod(cl, methStr);
    if (unlikely(fn.m_value == 0))
    {
        fprintf(stderr, "Object>>perform: Cannot invoke non-existent method %s.\n",
                reinterpret_cast<char*>(TranslateToRawPointer(&meth.As<tObject>()->m_data[1])));
        abort();
    }
    else
    {
        TValue* callbase = base + 2;
        callbase[0].m_value = reinterpret_cast<uint64_t>(fn.As());
        callbase[x_numSlotsForStackFrameHeader] = self;
        MakeInPlaceCall(callbase + x_numSlotsForStackFrameHeader, 1 /*numArgs*/, DEEGEN_LIB_FUNC_RETURN_CONTINUATION(TrivialReturnCont));
    }
}

static SOMClass* WARN_UNUSED ALWAYS_INLINE GetClassFromClassObject(TValue tv)
{
    TestAssert(tv.Is<tObject>());
    return reinterpret_cast<SOMClass*>(tv.As<tObject>()->m_data[0].m_value);
}

DEEGEN_DEFINE_LIB_FUNC(object_perform_in_superclass)
{
    SOM_LOG_PRIMITIVE_FREQ(object_perform_in_superclass);

    TValue* base = GetStackBase();
    TValue self = GetArg(0);
    TValue meth = GetArg(1);
    TValue sc = GetArg(2);
    VM* vm = VM_GetActiveVMForCurrentThread();
    SOMUniquedString methStr = GetUniquedStringFromVM(vm, meth);
    TestAssert(sc.Is<tObject>());
    HeapPtr<SOMClass> cl = TranslateToHeapPtr(GetClassFromClassObject(sc));
    GeneralHeapPointer<FunctionObject> fn = SOMClass::GetMethod(cl, methStr);
    if (fn.m_value == 0)
    {
        fprintf(stderr, "Object>>perform:inSuperclass: Cannot invoke non-existent method %s.\n",
                reinterpret_cast<char*>(TranslateToRawPointer(&meth.As<tObject>()->m_data[1])));
        abort();
    }
    else
    {
        TValue* callbase = base + 3;
        callbase[0].m_value = reinterpret_cast<uint64_t>(fn.As());
        callbase[x_numSlotsForStackFrameHeader] = self;
        MakeInPlaceCall(callbase + x_numSlotsForStackFrameHeader, 1 /*numArgs*/, DEEGEN_LIB_FUNC_RETURN_CONTINUATION(TrivialReturnCont));
    }
}

DEEGEN_DEFINE_LIB_FUNC(object_perform_with_args)
{
    SOM_LOG_PRIMITIVE_FREQ(object_perform_with_args);

    TValue* base = GetStackBase();
    TValue self = GetArg(0);
    TValue meth = GetArg(1);
    TValue args = GetArg(2);
    TestAssert(args.Is<tObject>() && args.As<tObject>()->m_arrayType == SOM_Array);
    HeapPtr<SOMObject> argsArr = args.As<tObject>();
    VM* vm = VM_GetActiveVMForCurrentThread();
    SOMUniquedString methStr = GetUniquedStringFromVM(vm, meth);
    HeapPtr<SOMClass> cl = GetSOMClassOfAny(self);
    GeneralHeapPointer<FunctionObject> fn = SOMClass::GetMethod(cl, methStr);
    if (fn.m_value == 0)
    {
        fprintf(stderr, "Object>>perform:withArguments: Cannot invoke non-existent method %s.\n",
                reinterpret_cast<char*>(TranslateToRawPointer(&meth.As<tObject>()->m_data[1])));
        abort();
    }
    else
    {
        TValue* callbase = base + 3;
        callbase[0].m_value = reinterpret_cast<uint64_t>(fn.As());
        size_t numArgs = argsArr->m_data[0].m_value;
        callbase[x_numSlotsForStackFrameHeader] = self;
        for (size_t i = 1; i <= numArgs; i++)
        {
            callbase[x_numSlotsForStackFrameHeader + i].m_value = argsArr->m_data[i].m_value;
        }
        MakeInPlaceCall(callbase + x_numSlotsForStackFrameHeader, numArgs + 1 /*numArgs*/, DEEGEN_LIB_FUNC_RETURN_CONTINUATION(TrivialReturnCont));
    }
}

DEEGEN_DEFINE_LIB_FUNC(object_perform_with_args_in_superclass)
{
    SOM_LOG_PRIMITIVE_FREQ(object_perform_with_args_in_superclass);

    TValue* base = GetStackBase();
    TValue self = GetArg(0);
    TValue meth = GetArg(1);
    TValue args = GetArg(2);
    TestAssert(args.Is<tObject>() && args.As<tObject>()->m_arrayType == SOM_Array);
    HeapPtr<SOMObject> argsArr = args.As<tObject>();
    TValue sc = GetArg(3);
    VM* vm = VM_GetActiveVMForCurrentThread();
    SOMUniquedString methStr = GetUniquedStringFromVM(vm, meth);
    TestAssert(sc.Is<tObject>());
    HeapPtr<SOMClass> cl = TranslateToHeapPtr(GetClassFromClassObject(sc));
    GeneralHeapPointer<FunctionObject> fn = SOMClass::GetMethod(cl, methStr);
    if (fn.m_value == 0)
    {
        fprintf(stderr, "Object>>perform:withArguments:inSuperclass: Cannot invoke non-existent method %s.\n",
                reinterpret_cast<char*>(TranslateToRawPointer(&meth.As<tObject>()->m_data[1])));
        abort();
    }
    else
    {
        TValue* callbase = base + 4;
        callbase[0].m_value = reinterpret_cast<uint64_t>(fn.As());
        size_t numArgs = argsArr->m_data[0].m_value;
        callbase[x_numSlotsForStackFrameHeader] = self;
        for (size_t i = 1; i <= numArgs; i++)
        {
            callbase[x_numSlotsForStackFrameHeader + i].m_value = argsArr->m_data[i].m_value;
        }
        MakeInPlaceCall(callbase + x_numSlotsForStackFrameHeader, numArgs + 1 /*numArgs*/, DEEGEN_LIB_FUNC_RETURN_CONTINUATION(TrivialReturnCont));
    }
}

DEEGEN_DEFINE_LIB_FUNC(object_instvarat)
{
    SOM_LOG_PRIMITIVE_FREQ(object_instvarat);

    TValue tv = GetArg(0);
    TestAssert(tv.Is<tObject>());
    HeapPtr<SOMObject> o = tv.As<tObject>();
    TestAssert(GetArg(1).Is<tInt32>());
    int32_t fieldIdx = GetArg(1).As<tInt32>() - 1;
    Return(TCGet(o->m_data[fieldIdx]));
}

DEEGEN_DEFINE_LIB_FUNC(object_instvaratput)
{
    SOM_LOG_PRIMITIVE_FREQ(object_instvaratput);

    TValue tv = GetArg(0);
    TestAssert(tv.Is<tObject>());
    HeapPtr<SOMObject> o = tv.As<tObject>();
    TestAssert(GetArg(1).Is<tInt32>());
    int32_t fieldIdx = GetArg(1).As<tInt32>() - 1;
    TValue valToPut = GetArg(2);
    o->m_data[fieldIdx].m_value = valToPut.m_value;
    Return(tv);
}

std::string_view ALWAYS_INLINE GetStringContentFromSOMString(TValue tv)
{
    TestAssert(tv.Is<tObject>() && tv.As<tObject>()->m_arrayType == SOM_String);
    HeapPtr<SOMObject> o = tv.As<tObject>();
    size_t len = o->m_data[0].m_value;
    char* buf = TranslateToRawPointer(reinterpret_cast<HeapPtr<char>>(&o->m_data[1]));
    return std::string_view(buf, len);
}

DEEGEN_DEFINE_LIB_FUNC(object_instvarnamed)
{
    SOM_LOG_PRIMITIVE_FREQ(object_instvarnamed);

    TValue tv = GetArg(0);
    TestAssert(tv.Is<tObject>());
    HeapPtr<SOMObject> o = tv.As<tObject>();
    std::string_view name = GetStringContentFromSOMString(GetArg(1));
    SOMClass* cl = TranslateToRawPointer(GetSOMClassOfAny(tv));
    for (size_t i = 0; i < cl->m_numFields; i++)
    {
        std::string_view fieldName = GetStringContentFromSOMString(cl->m_fields->m_data[i + 1]);
        if (name == fieldName)
        {
            Return(TCGet(o->m_data[i]));
        }
    }
    fprintf(stderr, "Invalid field name %s passed to instVarNamed.\n", name.data());
    abort();
}

DEEGEN_DEFINE_LIB_FUNC(object_class)
{
    SOM_LOG_PRIMITIVE_FREQ(object_class);

    TValue tv = GetArg(0);
    HeapPtr<SOMClass> cl = GetSOMClassOfAny(tv);
    TValue classObj = TValue::Create<tObject>(TranslateToHeapPtr(cl->m_classObject));
    Return(classObj);
}

DEEGEN_DEFINE_LIB_FUNC(class_new)
{
    SOM_LOG_PRIMITIVE_FREQ(class_new);
    TValue tv = GetArg(0);
    SOMClass* cl = GetClassFromClassObject(tv);
    SOMObject* o = cl->Instantiate();
    Return(TValue::Create<tObject>(TranslateToHeapPtr(o)));
}

DEEGEN_DEFINE_LIB_FUNC(class_name)
{
    SOM_LOG_PRIMITIVE_FREQ(class_name);

    TValue tv = GetArg(0);
    SOMClass* cl = GetClassFromClassObject(tv);
    Return(TValue::Create<tObject>(TranslateToHeapPtr(cl->m_name)));
}

DEEGEN_DEFINE_LIB_FUNC(class_superclass)
{
    SOM_LOG_PRIMITIVE_FREQ(class_superclass);

    TValue tv = GetArg(0);
    SOMClass* cl = GetClassFromClassObject(tv);
    if (cl->m_superClass == nullptr)
    {
        Return(TValue::Create<tNil>());
    }
    else
    {
        Return(TValue::Create<tObject>(TranslateToHeapPtr(cl->m_superClass->m_classObject)));
    }
}

DEEGEN_DEFINE_LIB_FUNC(class_fields)
{
    SOM_LOG_PRIMITIVE_FREQ(class_fields);

    TValue tv = GetArg(0);
    SOMClass* cl = GetClassFromClassObject(tv);
    Return(TValue::Create<tObject>(TranslateToHeapPtr(cl->m_fields)));
}

DEEGEN_DEFINE_LIB_FUNC(class_methods)
{
    SOM_LOG_PRIMITIVE_FREQ(class_methods);

    TValue tv = GetArg(0);
    SOMClass* cl = GetClassFromClassObject(tv);
    Return(TValue::Create<tObject>(TranslateToHeapPtr(cl->m_methods)));
}

DEEGEN_DEFINE_LIB_FUNC(block1_eval)
{
    SOM_LOG_PRIMITIVE_FREQ(block1_eval);

    TValue* base = GetStackBase();
    TValue tv = GetArg(0);
    SOMDetailEntityType fnTy = static_cast<SOMDetailEntityType>(tv.As<tFunction>()->m_invalidArrayType & 15);
    TestAssert(tv.Is<tFunction>() && (fnTy == SOM_BlockNoArg || fnTy == SOM_BlockNoArgImmSelf));
    HeapPtr<FunctionObject> fn = tv.As<tFunction>();
    TestAssert(fn->m_numUpvalues >= 1);
    TValue* callbase = base + 1;
    callbase[0].m_value = reinterpret_cast<uint64_t>(fn);
    if (fnTy == SOM_BlockNoArgImmSelf)
    {
        callbase[x_numSlotsForStackFrameHeader].m_value = fn->m_upvalues[0].m_value;
    }
    else
    {
        callbase[x_numSlotsForStackFrameHeader] = *reinterpret_cast<Upvalue*>(fn->m_upvalues[0].m_value)->m_ptr;
    }
    MakeInPlaceCall(callbase + x_numSlotsForStackFrameHeader, 1 /*numArgs*/, DEEGEN_LIB_FUNC_RETURN_CONTINUATION(TrivialReturnCont));
}

DEEGEN_DEFINE_LIB_FUNC(block2_eval)
{
    SOM_LOG_PRIMITIVE_FREQ(block2_eval);

    TValue* base = GetStackBase();
    TValue tv = GetArg(0);
    TValue arg = GetArg(1);
    SOMDetailEntityType fnTy = static_cast<SOMDetailEntityType>(tv.As<tFunction>()->m_invalidArrayType & 15);
    TestAssert(tv.Is<tFunction>() && (fnTy == SOM_BlockOneArg || fnTy == SOM_BlockOneArgImmSelf));
    HeapPtr<FunctionObject> fn = tv.As<tFunction>();
    TestAssert(fn->m_numUpvalues >= 1);
    TValue* callbase = base + 2;
    callbase[0].m_value = reinterpret_cast<uint64_t>(fn);
    if (fnTy == SOM_BlockOneArgImmSelf)
    {
        callbase[x_numSlotsForStackFrameHeader].m_value = fn->m_upvalues[0].m_value;
    }
    else
    {
        callbase[x_numSlotsForStackFrameHeader] = *reinterpret_cast<Upvalue*>(fn->m_upvalues[0].m_value)->m_ptr;
    }
    callbase[x_numSlotsForStackFrameHeader + 1] = arg;
    MakeInPlaceCall(callbase + x_numSlotsForStackFrameHeader, 2 /*numArgs*/, DEEGEN_LIB_FUNC_RETURN_CONTINUATION(TrivialReturnCont));
}

DEEGEN_DEFINE_LIB_FUNC(block3_eval)
{
    SOM_LOG_PRIMITIVE_FREQ(block3_eval);

    TValue* base = GetStackBase();
    TValue tv = GetArg(0);
    TValue arg1 = GetArg(1);
    TValue arg2 = GetArg(2);
    SOMDetailEntityType fnTy = static_cast<SOMDetailEntityType>(tv.As<tFunction>()->m_invalidArrayType & 15);
    TestAssert(tv.Is<tFunction>() && (fnTy == SOM_BlockTwoArgs || fnTy == SOM_BlockTwoArgsImmSelf));
    HeapPtr<FunctionObject> fn = tv.As<tFunction>();
    TestAssert(fn->m_numUpvalues >= 1);
    TValue* callbase = base + 3;
    callbase[0].m_value = reinterpret_cast<uint64_t>(fn);
    if (fnTy == SOM_BlockTwoArgsImmSelf)
    {
        callbase[x_numSlotsForStackFrameHeader].m_value = fn->m_upvalues[0].m_value;
    }
    else
    {
        callbase[x_numSlotsForStackFrameHeader] = *reinterpret_cast<Upvalue*>(fn->m_upvalues[0].m_value)->m_ptr;
    }
    callbase[x_numSlotsForStackFrameHeader + 1] = arg1;
    callbase[x_numSlotsForStackFrameHeader + 2] = arg2;
    MakeInPlaceCall(callbase + x_numSlotsForStackFrameHeader, 3 /*numArgs*/, DEEGEN_LIB_FUNC_RETURN_CONTINUATION(TrivialReturnCont));
}

// Setup stack to call block 'func' with only the implicit 'self' argument.
// If the block accepts more arguments, nil is passed in
// Return how many arguments 'func' actually has.
//
static size_t WARN_UNUSED ALWAYS_INLINE SetupStackForBlockNoArgCall(TValue* callbase, TValue func)
{
    HeapPtr<FunctionObject> fn = func.As<tFunction>();
    TestAssert(fn->m_numUpvalues >= 1);
    callbase[0].m_value = reinterpret_cast<uint64_t>(fn);
    SOMDetailEntityType fnTy = static_cast<SOMDetailEntityType>(fn->m_invalidArrayType & 15);
    size_t numArgs = (static_cast<size_t>(fnTy) - static_cast<size_t>(SOM_BlockNoArg)) / 2 + 1;

    if ((static_cast<uint8_t>(fnTy) & 1) == (static_cast<uint8_t>(SOM_BlockNoArgImmSelf) & 1))
    {
        callbase[x_numSlotsForStackFrameHeader].m_value = fn->m_upvalues[0].m_value;
    }
    else
    {
        callbase[x_numSlotsForStackFrameHeader] = *reinterpret_cast<Upvalue*>(fn->m_upvalues[0].m_value)->m_ptr;
    }
    callbase[x_numSlotsForStackFrameHeader + 1] = TValue::Create<tNil>();
    callbase[x_numSlotsForStackFrameHeader + 2] = TValue::Create<tNil>();
    return numArgs;
}

DEEGEN_FORWARD_DECLARE_LIB_FUNC_RETURN_CONTINUATION(block_whiletrue_evalbody);

DEEGEN_DEFINE_LIB_FUNC_CONTINUATION(block_whiletrue_evalcond)
{
    TValue* base = GetStackBase();
    TValue condBlock = GetArg(0);

    TValue* callbase = base + 2;
    size_t numArgs = SetupStackForBlockNoArgCall(callbase, condBlock);

    MakeInPlaceCall(callbase + x_numSlotsForStackFrameHeader, numArgs /*numArgs*/, DEEGEN_LIB_FUNC_RETURN_CONTINUATION(block_whiletrue_evalbody));
}

DEEGEN_DEFINE_LIB_FUNC_CONTINUATION(block_whiletrue_evalbody)
{
    TValue retVal = GetReturnValuesBegin()[0];
    if (retVal.m_value == TValue::Create<tBool>(true).m_value)
    {
        TValue* base = GetStackBase();
        TValue stmtBlock = GetArg(1);

        TValue* callbase = base + 2;
        size_t numArgs = SetupStackForBlockNoArgCall(callbase, stmtBlock);

        MakeInPlaceCall(callbase + x_numSlotsForStackFrameHeader, numArgs /*numArgs*/, DEEGEN_LIB_FUNC_RETURN_CONTINUATION(block_whiletrue_evalcond));
    }
    else
    {
        Return(TValue::Create<tNil>());
    }
}

DEEGEN_DEFINE_LIB_FUNC(block_whiletrue)
{
    SOM_LOG_PRIMITIVE_FREQ(block_whiletrue);

    TValue* base = GetStackBase();
    TValue condBlock = GetArg(0);

    TestAssert(condBlock.Is<tFunction>());
    TestAssert(GetArg(1).Is<tFunction>());

    TValue* callbase = base + 2;
    size_t numArgs = SetupStackForBlockNoArgCall(callbase, condBlock);

    MakeInPlaceCall(callbase + x_numSlotsForStackFrameHeader, numArgs /*numArgs*/, DEEGEN_LIB_FUNC_RETURN_CONTINUATION(block_whiletrue_evalbody));
}

DEEGEN_FORWARD_DECLARE_LIB_FUNC_RETURN_CONTINUATION(block_whilefalse_evalbody);

DEEGEN_DEFINE_LIB_FUNC_CONTINUATION(block_whilefalse_evalcond)
{
    TValue* base = GetStackBase();
    TValue condBlock = GetArg(0);

    TValue* callbase = base + 2;
    size_t numArgs = SetupStackForBlockNoArgCall(callbase, condBlock);

    MakeInPlaceCall(callbase + x_numSlotsForStackFrameHeader, numArgs /*numArgs*/, DEEGEN_LIB_FUNC_RETURN_CONTINUATION(block_whilefalse_evalbody));
}

DEEGEN_DEFINE_LIB_FUNC_CONTINUATION(block_whilefalse_evalbody)
{
    TValue retVal = GetReturnValuesBegin()[0];
    if (retVal.m_value == TValue::Create<tBool>(false).m_value)
    {
        TValue* base = GetStackBase();
        TValue stmtBlock = GetArg(1);

        TValue* callbase = base + 2;
        size_t numArgs = SetupStackForBlockNoArgCall(callbase, stmtBlock);

        MakeInPlaceCall(callbase + x_numSlotsForStackFrameHeader, numArgs /*numArgs*/, DEEGEN_LIB_FUNC_RETURN_CONTINUATION(block_whilefalse_evalcond));
    }
    else
    {
        Return(TValue::Create<tNil>());
    }
}

DEEGEN_DEFINE_LIB_FUNC(block_whilefalse)
{
    SOM_LOG_PRIMITIVE_FREQ(block_whilefalse);

    TValue* base = GetStackBase();
    TValue condBlock = GetArg(0);

    TestAssert(condBlock.Is<tFunction>());
    TestAssert(GetArg(1).Is<tFunction>());

    TValue* callbase = base + 2;
    size_t numArgs = SetupStackForBlockNoArgCall(callbase, condBlock);

    MakeInPlaceCall(callbase + x_numSlotsForStackFrameHeader, numArgs /*numArgs*/, DEEGEN_LIB_FUNC_RETURN_CONTINUATION(block_whilefalse_evalbody));
}

DEEGEN_DEFINE_LIB_FUNC(method_signature)
{
    SOM_LOG_PRIMITIVE_FREQ(method_signature);

    TValue tv = GetArg(0);
    Return(TCGet(tv.As<tObject>()->m_data[1]));
}

DEEGEN_DEFINE_LIB_FUNC(method_holder)
{
    SOM_LOG_PRIMITIVE_FREQ(method_holder);

    TValue tv = GetArg(0);
    Return(TCGet(tv.As<tObject>()->m_data[0]));
}

DEEGEN_DEFINE_LIB_FUNC(method_invoke_on_with)
{
    SOM_LOG_PRIMITIVE_FREQ(method_invoke_on_with);

    TValue* base = GetStackBase();
    TValue meth = GetArg(0);
    TValue self = GetArg(1);
    TValue args = GetArg(2);

    TestAssert(args.Is<tObject>() && args.As<tObject>()->m_arrayType == SOM_Array);
    HeapPtr<SOMObject> argsArr = args.As<tObject>();

    TValue func = TCGet(meth.As<tObject>()->m_data[2]);
    TestAssert(func.Is<tFunction>());

    TValue* callbase = base + 3;
    callbase[0].m_value = reinterpret_cast<uint64_t>(func.As<tFunction>());
    size_t numArgs = argsArr->m_data[0].m_value;
    callbase[x_numSlotsForStackFrameHeader] = self;
    for (size_t i = 1; i <= numArgs; i++)
    {
        callbase[x_numSlotsForStackFrameHeader + i].m_value = argsArr->m_data[i].m_value;
    }
    MakeInPlaceCall(callbase + x_numSlotsForStackFrameHeader, numArgs + 1 /*numArgs*/, DEEGEN_LIB_FUNC_RETURN_CONTINUATION(TrivialReturnCont));
}

DEEGEN_DEFINE_LIB_FUNC(system_global)
{
    SOM_LOG_PRIMITIVE_FREQ(system_global);

    TValue tv = GetArg(1);
    VM* vm = VM_GetActiveVMForCurrentThread();
    std::string_view globalName = GetStringContentFromSOMString(tv);
    size_t idx = 0;
    // system.global silently returns nil for global that doesn't exist.
    //
    if (!vm->GetSlotForGlobalNoCreate(globalName, idx /*out*/))
    {
        Return(TValue::Create<tNil>());
    }
    TValue res = vm->VM_GetGlobal(SafeIntegerCast<uint16_t>(idx));
    if (res.m_value == TValue::CreateImpossibleValue().m_value)
    {
        Return(TValue::Create<tNil>());
    }
    Return(res);
}

DEEGEN_DEFINE_LIB_FUNC(system_globalput)
{
    SOM_LOG_PRIMITIVE_FREQ(system_globalput);

    TValue self = GetArg(0);
    TValue tv = GetArg(1);
    TValue valToPut = GetArg(2);
    VM* vm = VM_GetActiveVMForCurrentThread();
    std::string_view globalName = GetStringContentFromSOMString(tv);
    size_t slot = vm->GetSlotForGlobal(globalName);
    vm->m_somGlobals[slot] = valToPut;
    Return(self);
}

DEEGEN_DEFINE_LIB_FUNC(system_hasglobal)
{
    SOM_LOG_PRIMITIVE_FREQ(system_hasglobal);

    TValue tv = GetArg(1);
    VM* vm = VM_GetActiveVMForCurrentThread();
    std::string_view globalName = GetStringContentFromSOMString(tv);
    size_t idx = 0;
    if (!vm->GetSlotForGlobalNoCreate(globalName, idx /*out*/))
    {
        Return(TValue::Create<tBool>(false));
    }
    TValue res = vm->VM_GetGlobal(SafeIntegerCast<uint16_t>(idx));
    if (res.m_value == TValue::CreateImpossibleValue().m_value)
    {
        Return(TValue::Create<tBool>(false));
    }
    Return(TValue::Create<tBool>(true));
}

DEEGEN_DEFINE_LIB_FUNC(system_load)
{
    SOM_LOG_PRIMITIVE_FREQ(system_load);

    TValue tv = GetArg(1);
    std::string_view className = GetStringContentFromSOMString(tv);
    SOMClass* cl = SOMCompileFile(std::string(className));
    Return(TValue::Create<tObject>(TranslateToHeapPtr(cl->m_classObject)));
}

DEEGEN_DEFINE_LIB_FUNC(system_exit)
{
    SOM_LOG_PRIMITIVE_FREQ(system_exit);

    int32_t err = GetArg(1).As<tInt32>();

    if (err != 0)
    {
        fprintf(stderr, "[SOM] system.exit called with error code %d. Stacktrace:\n", static_cast<int>(err));
        PrintSOMStackTrace(VM_GetActiveVMForCurrentThread()->GetStderr(), GetStackFrameHeader());
    }

    exit(err);
}

DEEGEN_DEFINE_LIB_FUNC(system_printstacktrace)
{
    SOM_LOG_PRIMITIVE_FREQ(system_printstacktrace);

    PrintSOMStackTrace(VM_GetActiveVMForCurrentThread()->GetStdout(), GetStackFrameHeader());
    Return();
}

DEEGEN_DEFINE_LIB_FUNC(system_printstring)
{
    SOM_LOG_PRIMITIVE_FREQ(system_printstring);

    TValue self = GetArg(0);
    TValue tv = GetArg(1);
    std::string_view str = GetStringContentFromSOMString(tv);
    FILE* f = VM_GetActiveVMForCurrentThread()->GetStdout();
    for (char c : str) { fputc(c, f); }
    Return(self);
}

DEEGEN_DEFINE_LIB_FUNC(system_printnewline)
{
    SOM_LOG_PRIMITIVE_FREQ(system_printnewline);

    TValue self = GetArg(0);
    FILE* f = VM_GetActiveVMForCurrentThread()->GetStdout();
    fprintf(f, "\n");
    Return(self);
}

DEEGEN_DEFINE_LIB_FUNC(system_errorprint)
{
    SOM_LOG_PRIMITIVE_FREQ(system_errorprint);

    TValue self = GetArg(0);
    TValue tv = GetArg(1);
    std::string_view str = GetStringContentFromSOMString(tv);
    FILE* f = VM_GetActiveVMForCurrentThread()->GetStderr();
    for (char c : str) { fputc(c, f); }
    Return(self);
}

DEEGEN_DEFINE_LIB_FUNC(system_errorprintln)
{
    SOM_LOG_PRIMITIVE_FREQ(system_errorprintln);

    TValue self = GetArg(0);
    TValue tv = GetArg(1);
    std::string_view str = GetStringContentFromSOMString(tv);
    FILE* f = VM_GetActiveVMForCurrentThread()->GetStderr();
    for (char c : str) { fputc(c, f); }
    fprintf(f, "\n");
    Return(self);
}

DEEGEN_DEFINE_LIB_FUNC(system_elapsed_milliseconds)
{
    SOM_LOG_PRIMITIVE_FREQ(system_elapsed_milliseconds);

    VM* vm = VM_GetActiveVMForCurrentThread();
    double result = vm->m_vmStartTime.GetElapsedTime();
    Return(TValue::Create<tInt32>(static_cast<int32_t>(result * 1000)));
}

DEEGEN_DEFINE_LIB_FUNC(system_elapsed_microseconds)
{
    SOM_LOG_PRIMITIVE_FREQ(system_elapsed_microseconds);

    VM* vm = VM_GetActiveVMForCurrentThread();
    double result = vm->m_vmStartTime.GetElapsedTime();
    Return(TValue::Create<tInt32>(static_cast<int32_t>(result * 1000000)));
}

DEEGEN_DEFINE_LIB_FUNC(system_fullgc)
{
    SOM_LOG_PRIMITIVE_FREQ(system_fullgc);

    Return(TValue::Create<tBool>(true));
}

DEEGEN_DEFINE_LIB_FUNC(system_loadfile)
{
    SOM_LOG_PRIMITIVE_FREQ(system_loadfile);

    TValue tv = GetArg(1);
    std::string_view fileName = GetStringContentFromSOMString(tv);
    std::ifstream file(fileName.data(), std::ifstream::in);
    if (file.is_open())
    {
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string contents = buffer.str();
        SOMObject* res = SOMObject::AllocateString(contents);
        Return(TValue::Create<tObject>(TranslateToHeapPtr(res)));
    }
    else
    {
        Return(TValue::Create<tNil>());
    }
}

DEEGEN_DEFINE_LIB_FUNC(array_at)
{
    SOM_LOG_PRIMITIVE_FREQ(array_at);

    TValue tv = GetArg(0);
    int32_t idx = GetArg(1).As<tInt32>();
    TestAssert(tv.Is<tObject>() && tv.As<tObject>()->m_arrayType == SOM_Array);
    HeapPtr<SOMObject> o = tv.As<tObject>();
    if (unlikely(idx <= 0 || idx > static_cast<int64_t>(o->m_data[0].m_value)))
    {
        fprintf(stderr, "Array access out of bound: index = %d, size = %d\n",
                static_cast<int>(idx), static_cast<int>(o->m_data[0].m_value));
        abort();
    }
    Return(TCGet(o->m_data[idx]));
}

DEEGEN_DEFINE_LIB_FUNC(array_at_put)
{
    SOM_LOG_PRIMITIVE_FREQ(array_at_put);

    TValue tv = GetArg(0);
    int32_t idx = GetArg(1).As<tInt32>();
    TValue valToPut = GetArg(2);
    TestAssert(tv.Is<tObject>() && tv.As<tObject>()->m_arrayType == SOM_Array);
    HeapPtr<SOMObject> o = tv.As<tObject>();
    if (unlikely(idx <= 0 || idx > static_cast<int64_t>(o->m_data[0].m_value)))
    {
        fprintf(stderr, "Array write out of bound: index = %d, size = %d\n",
                static_cast<int>(idx), static_cast<int>(o->m_data[0].m_value));
        abort();
    }
    o->m_data[idx].m_value = valToPut.m_value;
    Return(tv);
}

DEEGEN_DEFINE_LIB_FUNC(array_length)
{
    SOM_LOG_PRIMITIVE_FREQ(array_length);

    TValue tv = GetArg(0);
    TestAssert(tv.Is<tObject>() && tv.As<tObject>()->m_arrayType == SOM_Array);
    HeapPtr<SOMObject> o = tv.As<tObject>();
    Return(TValue::Create<tInt32>(static_cast<int32_t>(o->m_data[0].m_value)));
}

DEEGEN_DEFINE_LIB_FUNC(array_new)
{
    SOM_LOG_PRIMITIVE_FREQ(array_new);

    int32_t len = GetArg(1).As<tInt32>();
    if (len < 0)
    {
        fprintf(stderr, "Cannot create array of negative length %d.\n", static_cast<int>(len));
        abort();
    }
    SOMObject* o = SOMObject::AllocateArray(static_cast<size_t>(len));
    Return(TValue::Create<tObject>(TranslateToHeapPtr(o)));
}

DEEGEN_DEFINE_LIB_FUNC(array_copy)
{
    SOM_LOG_PRIMITIVE_FREQ(array_copy);

    TValue tv = GetArg(0);
    TestAssert(tv.Is<tObject>() && tv.As<tObject>()->m_arrayType == SOM_Array);
    HeapPtr<SOMObject> o = tv.As<tObject>();
    SOMObject* r = TranslateToRawPointer(o)->ShallowCopyArray();
    Return(TValue::Create<tObject>(TranslateToHeapPtr(r)));
}

DEEGEN_DEFINE_LIB_FUNC(string_concatenate)
{
    SOM_LOG_PRIMITIVE_FREQ(string_concatenate);

    TValue lhs = GetArg(0);
    TValue rhs = GetArg(1);
    TestAssert(lhs.Is<tObject>() && lhs.As<tObject>()->m_arrayType == SOM_String);
    TestAssert(rhs.Is<tObject>() && rhs.As<tObject>()->m_arrayType == SOM_String);
    HeapPtr<SOMObject> r = SOMObject::DoStringConcat(lhs.As<tObject>(), rhs.As<tObject>());
    Return(TValue::Create<tObject>(r));
}

DEEGEN_DEFINE_LIB_FUNC(string_assymbol)
{
    SOM_LOG_PRIMITIVE_FREQ(string_assymbol);

    TValue tv = GetArg(0);
    TestAssert(tv.Is<tObject>() && tv.As<tObject>()->m_arrayType == SOM_String);
    VM* vm = VM_GetActiveVMForCurrentThread();
    size_t ord = vm->m_interner.InternString(GetStringContentFromSOMString(tv));
    SOMObject* o = vm->GetInternedSymbol(ord);
    Return(TValue::Create<tObject>(TranslateToHeapPtr(o)));
}

DEEGEN_DEFINE_LIB_FUNC(string_hashcode)
{
    SOM_LOG_PRIMITIVE_FREQ(string_hashcode);

    TValue tv = GetArg(0);
    std::string_view str = GetStringContentFromSOMString(tv);

    // Use exactly SOM++'s hash implementation
    //
    size_t length = str.length();
    uint64_t hash = 5381U;

    for (size_t i = 0; i < length; i++) {
        hash = ((hash << 5U) + hash) + static_cast<uint64_t>(str[i]);
    }

    Return(TValue::Create<tInt32>(static_cast<int32_t>(hash)));
}

DEEGEN_DEFINE_LIB_FUNC(string_length)
{
    SOM_LOG_PRIMITIVE_FREQ(string_length);

    TValue tv = GetArg(0);
    TestAssert(tv.Is<tObject>() && tv.As<tObject>()->m_arrayType == SOM_String);
    Return(TValue::Create<tInt32>(static_cast<int32_t>(tv.As<tObject>()->m_data[0].m_value)));
}

DEEGEN_DEFINE_LIB_FUNC(string_equal)
{
    SOM_LOG_PRIMITIVE_FREQ(string_equal);

    TValue lhs = GetArg(0);
    TValue rhs = GetArg(1);
    TestAssert(lhs.Is<tObject>() && lhs.As<tObject>()->m_arrayType == SOM_String);

    if (lhs.m_value == rhs.m_value) { Return(TValue::Create<tBool>(true)); }

    if (!rhs.Is<tObject>()) { Return(TValue::Create<tBool>(false)); }
    HeapPtr<SOMObject> r = rhs.As<tObject>();
    if (r->m_arrayType != SOM_String) { Return(TValue::Create<tBool>(false)); }

    HeapPtr<SOMObject> l = lhs.As<tObject>();
    size_t len = r->m_data[0].m_value;
    if (len != l->m_data[0].m_value) { Return(TValue::Create<tBool>(false)); }

    VM* vm = VM_GetActiveVMForCurrentThread();
    int res = memcmp(TranslateToRawPointer(vm, &l->m_data[1]), TranslateToRawPointer(vm, &r->m_data[1]), len);
    Return(TValue::Create<tBool>(res == 0));
}

DEEGEN_DEFINE_LIB_FUNC(string_primsubstring)
{
    SOM_LOG_PRIMITIVE_FREQ(string_primsubstring);

    TValue tv = GetArg(0);
    int32_t start = GetArg(1).As<tInt32>();
    int32_t end = GetArg(2).As<tInt32>();
    TestAssert(tv.Is<tObject>() && tv.As<tObject>()->m_arrayType == SOM_String);
    std::string_view str = GetStringContentFromSOMString(tv);
    TestAssert(1 <= start && start <= end + 1 && end <= static_cast<int64_t>(str.length()));
    SOMObject* r = SOMObject::AllocateString(str.substr(static_cast<size_t>(start - 1), static_cast<size_t>(end - start + 1)));
    Return(TValue::Create<tObject>(TranslateToHeapPtr(r)));
}

DEEGEN_DEFINE_LIB_FUNC(string_charat)
{
    SOM_LOG_PRIMITIVE_FREQ(string_charat);

    TValue tv = GetArg(0);
    int32_t idx = GetArg(1).As<tInt32>();
    TestAssert(tv.Is<tObject>() && tv.As<tObject>()->m_arrayType == SOM_String);
    std::string_view str = GetStringContentFromSOMString(tv);
    if (unlikely(idx <= 0 || idx > static_cast<int64_t>(str.length())))
    {
        fprintf(stderr, "String charAt out of bound: index = %d, string length = %d\n",
                static_cast<int>(idx), static_cast<int>(str.length()));
        abort();
    }
    SOMObject* r = SOMObject::AllocateString(str.substr(static_cast<size_t>(idx - 1), 1));
    Return(TValue::Create<tObject>(TranslateToHeapPtr(r)));
}

DEEGEN_DEFINE_LIB_FUNC(string_iswhitespace)
{
    SOM_LOG_PRIMITIVE_FREQ(string_iswhitespace);

    TValue tv = GetArg(0);
    TestAssert(tv.Is<tObject>() && tv.As<tObject>()->m_arrayType == SOM_String);
    std::string_view str = GetStringContentFromSOMString(tv);

    size_t len = str.length();
    if (len == 0)
    {
        Return(TValue::Create<tBool>(false));
    }

    for (size_t i = 0; i < len; i++)
    {
        if (isspace(str[i]) == 0)
        {
            Return(TValue::Create<tBool>(false));
        }
    }
    Return(TValue::Create<tBool>(true));
}

DEEGEN_DEFINE_LIB_FUNC(string_isletters)
{
    SOM_LOG_PRIMITIVE_FREQ(string_isletters);

    TValue tv = GetArg(0);
    TestAssert(tv.Is<tObject>() && tv.As<tObject>()->m_arrayType == SOM_String);
    std::string_view str = GetStringContentFromSOMString(tv);

    size_t len = str.length();
    if (len == 0)
    {
        Return(TValue::Create<tBool>(false));
    }

    for (size_t i = 0; i < len; i++)
    {
        if (isalpha(str[i]) == 0)
        {
            Return(TValue::Create<tBool>(false));
        }
    }
    Return(TValue::Create<tBool>(true));
}

DEEGEN_DEFINE_LIB_FUNC(string_isdigits)
{
    SOM_LOG_PRIMITIVE_FREQ(string_isdigits);

    TValue tv = GetArg(0);
    TestAssert(tv.Is<tObject>() && tv.As<tObject>()->m_arrayType == SOM_String);
    std::string_view str = GetStringContentFromSOMString(tv);

    size_t len = str.length();
    if (len == 0)
    {
        Return(TValue::Create<tBool>(false));
    }

    for (size_t i = 0; i < len; i++)
    {
        if (isdigit(str[i]) == 0)
        {
            Return(TValue::Create<tBool>(false));
        }
    }
    Return(TValue::Create<tBool>(true));
}

DEEGEN_DEFINE_LIB_FUNC(symbol_asstring)
{
    SOM_LOG_PRIMITIVE_FREQ(symbol_asstring);

    TValue tv = GetArg(0);
    TestAssert(tv.Is<tObject>() && tv.As<tObject>()->m_arrayType == SOM_String);
    VM* vm = VM_GetActiveVMForCurrentThread();
    size_t ord = vm->m_interner.InternString(GetStringContentFromSOMString(tv));
    SOMObject* o = vm->GetInternedString(ord);
    Return(TValue::Create<tObject>(TranslateToHeapPtr(o)));
}

DEEGEN_DEFINE_LIB_FUNC(unimplemented_primitive)
{
    HeapPtr<FunctionObject> f = GetStackFrameHeader()->m_func;
    TestAssert(f->m_numUpvalues == 1);
    TValue str = TCGet(f->m_upvalues[0]);
    TestAssert(str.Is<tObject>());
    HeapPtr<SOMObject> o = str.As<tObject>();
    TestAssert(o->m_arrayType == SOM_String);
    fprintf(stderr, "Unimplemented primitive %s!\n", reinterpret_cast<char*>(TranslateToRawPointer(&o->m_data[1])));
    abort();
}

DEEGEN_END_LIB_FUNC_DEFINITIONS
