#include "som_primitives_container.h"
#include "runtime_utils.h"
#include "api_define_lib_function.h"
#include "vm.h"
#include "som_class.h"

DEEGEN_FORWARD_DECLARE_LIB_FUNC(unimplemented_primitive);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_add);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_minus);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_star);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_rem);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_bitwisexor);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_leftshift);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_unsignedrightshift);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_slash);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_slashslash);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_percent);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_and);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_equal);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_equalequal);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_lowerthan);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_lowerequal);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_greaterthan);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_greaterequal);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_unequal);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_asstring);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_asdouble);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_as32bitsigned);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_as32bitunsigned);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_sqrt);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_atrandom);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_fromstring);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_abs);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_min);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_max);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(integer_range);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_add);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_sub);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_star);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_slashslash);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_percent);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_sin);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_cos);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_equal);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_unequal);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_lowerthan);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_lowerequal);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_greaterthan);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_greaterequal);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_min);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_max);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_asstring);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_round);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_asinteger);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_sqrt);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_positiveinfinity);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(double_fromstring);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(object_equalequal);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(object_sizebytes);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(object_hashcode);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(object_inspect);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(object_halt);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(object_perform);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(object_perform_in_superclass);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(object_perform_with_args);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(object_perform_with_args_in_superclass);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(object_instvarat);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(object_instvaratput);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(object_instvarnamed);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(object_class);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(class_new);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(class_name);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(class_superclass);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(class_fields);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(class_methods);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(block1_eval);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(block2_eval);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(block3_eval);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(block_whiletrue);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(block_whilefalse);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(method_signature);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(method_holder);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(method_invoke_on_with);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(system_global);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(system_globalput);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(system_hasglobal);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(system_load);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(system_exit);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(system_printstacktrace);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(system_printstring);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(system_printnewline);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(system_errorprint);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(system_errorprintln);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(system_elapsed_milliseconds);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(system_elapsed_microseconds);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(system_fullgc);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(system_loadfile);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(array_at);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(array_at_put);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(array_length);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(array_new);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(array_copy);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(string_concatenate);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(string_assymbol);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(string_hashcode);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(string_length);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(string_equal);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(string_primsubstring);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(string_charat);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(string_iswhitespace);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(string_isletters);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(string_isdigits);
DEEGEN_FORWARD_DECLARE_LIB_FUNC(symbol_asstring);

SOMPrimitivesContainer::SOMPrimitivesContainer()
{
    Add("Integer", "+", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_add));
    Add("Integer", "-", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_minus));
    Add("Integer", "*", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_star));
    Add("Integer", "rem:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_rem));
    Add("Integer", "bitXor:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_bitwisexor));
    Add("Integer", "<<", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_leftshift));
    Add("Integer", ">>>", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_unsignedrightshift));
    Add("Integer", "/", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_slash));
    Add("Integer", "//", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_slashslash));
    Add("Integer", "%", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_percent));
    Add("Integer", "&", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_and));
    Add("Integer", "=", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_equal));
    Add("Integer", "==", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_equalequal));
    Add("Integer", "<", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_lowerthan));
    Add("Integer", "asString", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_asstring));
    Add("Integer", "asDouble", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_asdouble));
    Add("Integer", "as32BitSignedValue", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_as32bitsigned));
    Add("Integer", "as32BitUnsignedValue", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_as32bitunsigned));
    Add("Integer", "sqrt", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_sqrt));
    Add("Integer", "atRandom", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_atrandom));
    Add("Integer", "fromString:", true, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_fromstring));
    Add("Integer", "<=", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_lowerequal));
    Add("Integer", ">", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_greaterthan));
    Add("Integer", ">=", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_greaterequal));
    Add("Integer", "<>", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_unequal));
    Add("Integer", "~=", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_unequal));
    Add("Integer", "abs", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_abs));
    Add("Integer", "min:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_min));
    Add("Integer", "max:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_max));
    Add("Integer", "to:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(integer_range));

    Add("Double", "+", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_add));
    Add("Double", "-", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_sub));
    Add("Double", "*", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_star));
    Add("Double", "cos", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_cos));
    Add("Double", "sin", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_sin));
    Add("Double", "//", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_slashslash));
    Add("Double", "%", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_percent));
    Add("Double", "=", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_equal));
    Add("Double", "<", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_lowerthan));
    Add("Double", "asString", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_asstring));
    Add("Double", "sqrt", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_sqrt));
    Add("Double", "round", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_round));
    Add("Double", "asInteger", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_asinteger));
    Add("Double", "PositiveInfinity", true, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_positiveinfinity));
    Add("Double", "fromString:", true, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_fromstring));
    Add("Double", "<=", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_lowerequal));
    Add("Double", ">", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_greaterthan));
    Add("Double", ">=", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_greaterequal));
    Add("Double", "<>", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_unequal));
    Add("Double", "~=", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_unequal));
    Add("Double", "min:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_min));
    Add("Double", "max:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(double_max));

    Add("Object", "==", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(object_equalequal));
    Add("Object", "objectSize", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(object_sizebytes));
    Add("Object", "hashcode", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(object_hashcode));
    Add("Object", "inspect", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(object_inspect));
    Add("Object", "halt", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(object_halt));
    Add("Object", "perform:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(object_perform));
    Add("Object", "perform:withArguments:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(object_perform_with_args));
    Add("Object", "perform:inSuperclass:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(object_perform_in_superclass));
    Add("Object", "perform:withArguments:inSuperclass:",
        false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(object_perform_with_args_in_superclass));
    Add("Object", "instVarAt:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(object_instvarat));
    Add("Object", "instVarAt:put:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(object_instvaratput));
    Add("Object", "instVarNamed:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(object_instvarnamed));

    Add("Object", "class", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(object_class));

    Add("Class", "new", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(class_new));
    Add("Class", "name", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(class_name));
    Add("Class", "superclass", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(class_superclass));
    Add("Class", "fields", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(class_fields));
    Add("Class", "methods", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(class_methods));

    Add("Block1", "value", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(block1_eval));

    Add("Block2", "value:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(block2_eval));

    Add("Block3", "value:with:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(block3_eval));

    Add("Block", "whileTrue:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(block_whiletrue));
    Add("Block", "whileFalse:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(block_whilefalse));

    Add("Method", "signature", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(method_signature));
    Add("Method", "holder", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(method_holder));
    Add("Method", "invokeOn:with:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(method_invoke_on_with));

    Add("Primitive", "signature", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(method_signature));
    Add("Primitive", "holder", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(method_holder));
    Add("Primitive", "invokeOn:with:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(method_invoke_on_with));

    Add("System", "global:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(system_global));
    Add("System", "global:put:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(system_globalput));
    Add("System", "hasGlobal:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(system_hasglobal));
    Add("System", "load:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(system_load));
    Add("System", "exit:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(system_exit));
    Add("System", "printStackTrace", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(system_printstacktrace));
    Add("System", "printString:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(system_printstring));
    Add("System", "printNewline", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(system_printnewline));
    Add("System", "errorPrint:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(system_errorprint));
    Add("System", "errorPrintln:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(system_errorprintln));
    Add("System", "time", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(system_elapsed_milliseconds));
    Add("System", "ticks", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(system_elapsed_microseconds));
    Add("System", "fullGC", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(system_fullgc));
    Add("System", "loadFile:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(system_loadfile));

    Add("Array", "new:", true, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(array_new));
    Add("Array", "at:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(array_at));
    Add("Array", "at:put:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(array_at_put));
    Add("Array", "length", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(array_length));
    Add("Array", "copy", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(array_copy));

    Add("String", "concatenate:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(string_concatenate));
    Add("String", "asSymbol", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(string_assymbol));
    Add("String", "hashcode", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(string_hashcode));
    Add("String", "length", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(string_length));
    Add("String", "=", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(string_equal));
    Add("String", "primSubstringFrom:to:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(string_primsubstring));
    Add("String", "isWhiteSpace", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(string_iswhitespace));
    Add("String", "isLetters", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(string_isletters));
    Add("String", "isDigits", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(string_isdigits));
    Add("String", "charAt:", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(string_charat));

    Add("Symbol", "asString", false, DEEGEN_CODE_POINTER_FOR_LIB_FUNC(symbol_asstring));
}

void SOMPrimitivesContainer::Add(std::string_view className, std::string_view methName, bool isClassSide, void* func)
{
    auto& it = m_map[static_cast<size_t>(isClassSide)][className][methName];
    TestAssert(it.m_implPtr == nullptr && it.m_fnObj == nullptr && func != nullptr);
    it.m_implPtr = func;
}

HeapPtr<FunctionObject> WARN_UNUSED GetUnimplementedPrimitive(std::string_view className, std::string_view methName, bool isClassSide)
{
    VM* vm = VM_GetActiveVMForCurrentThread();
    void* funcPtr = DEEGEN_CODE_POINTER_FOR_LIB_FUNC(unimplemented_primitive);
    HeapPtr<FunctionObject> fn = FunctionObject::CreateCFunc(vm, ExecutableCode::CreateCFunction(vm, funcPtr), 1 /*numUpvalues*/).As();
    std::string descStr = std::string(className) + "." + std::string(methName) + (isClassSide ? " (classSide)" : " (instanceSide)");
    SOMObject* str = SOMObject::AllocateString(descStr);
    TValue strTv = TValue::Create<tObject>(TranslateToHeapPtr(str));
    fn->m_upvalues[0].m_value = strTv.m_value;
    return fn;
}

void SOMPrimitivesContainer::Element::InitFnObj()
{
    TestAssert(m_implPtr != nullptr && m_fnObj == nullptr);
    VM* vm = VM_GetActiveVMForCurrentThread();
    m_fnObj = FunctionObject::CreateCFunc(vm, ExecutableCode::CreateCFunction(vm, m_implPtr)).As();
}

HeapPtr<FunctionObject> WARN_UNUSED SOMPrimitivesContainer::Get(std::string_view className, std::string_view methName, bool isClassSide)
{
    auto& it = m_map[static_cast<size_t>(isClassSide)][className][methName];
    if (it.m_implPtr == nullptr)
    {
        // fprintf(stderr, "WARNING: unimplemented primitive %s.%s %s\n",
        //         std::string(className).c_str(), std::string(methName).c_str(), (isClassSide ? "(classSide)" : "(instanceSide)"));
        return GetUnimplementedPrimitive(className, methName, isClassSide);
    }
    if (it.m_fnObj == nullptr)
    {
        it.InitFnObj();
    }
    TestAssert(it.m_fnObj != nullptr);
    //fprintf(stderr, "primitive %s %s at 0x%llx\n",
    //        std::string(className).c_str(), std::string(methName).c_str(), static_cast<unsigned long long>(reinterpret_cast<uint64_t>(it.m_fnObj)));
    return it.m_fnObj;
}
