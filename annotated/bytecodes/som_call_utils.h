#pragma once

#include "api_define_bytecode.h"
#include "deegen_api.h"
#include "api_inline_cache.h"

#include "runtime_utils.h"
#include "som_class.h"
#include "som_utils.h"

template<bool isSuper>
[[maybe_unused]] static std::pair<SOMMethodLookupResultKind, HeapPtr<FunctionObject>> ALWAYS_INLINE LookupObjectMethodImpl(
    HeapPtr<UserHeapGcObjectHeader> baseEntity, TValue methTv, HeapPtr<SOMClass> superClass = nullptr)
{
    // Ugly: 'methTv' is always a SOMUniquedString disguised as a TValue..
    //
    SOMUniquedString meth { .m_id = static_cast<uint32_t>(methTv.m_value), .m_hash = static_cast<uint32_t>(methTv.m_value >> 32) };
    HeapPtr<SOMObject> base = reinterpret_cast<HeapPtr<SOMObject>>(baseEntity);
    ICHandler* ic = MakeInlineCache();
    ic->AddKey(base->m_hiddenClass).SpecifyImpossibleValue(0);
    ic->FuseICIntoInterpreterOpcode();
    return ic->Body(
        [ic, base, meth, superClass]() -> std::pair<SOMMethodLookupResultKind, HeapPtr<FunctionObject>> {
            if (unlikely(base->m_type != HeapEntityType::Object))
            {
                return std::make_pair(SOM_CallBaseNotObject, Undef<HeapPtr<FunctionObject>>());
            }
            HeapPtr<SOMClass> hc = isSuper ? superClass : SystemHeapPointer<SOMClass>(base->m_hiddenClass).As();
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
}

[[maybe_unused]] static GeneralHeapPointer<FunctionObject> ALWAYS_INLINE LookupMethodGeneralImpl(TValue self, TValue methTv)
{
    HeapPtr<SOMClass> cl = GetSOMClassOfAny(self);
    SOMUniquedString meth { .m_id = static_cast<uint32_t>(methTv.m_value), .m_hash = static_cast<uint32_t>(methTv.m_value >> 32) };
    // PrintTValue(stderr, self);
    // fprintf(stderr, " %s\n", VM_GetActiveVMForCurrentThread()->m_interner.Get(meth.m_id).data());
    return SOMClass::GetMethod(cl, meth);
}

template<auto RetCont>
[[maybe_unused]] static void NO_RETURN ALWAYS_INLINE HandleMethodNotFoundImpl(TValue self, TValue methTv, TValue* argStart, size_t numArgs)
{
    VM* vm = VM_GetActiveVMForCurrentThread();
    HeapPtr<SOMClass> cl = GetSOMClassOfAny(self);
    GeneralHeapPointer<FunctionObject> handler = SOMClass::GetMethod(cl, vm->m_doesNotUnderstandHandler);
    TestAssert(handler.m_value != 0);
    SOMUniquedString meth { .m_id = static_cast<uint32_t>(methTv.m_value), .m_hash = static_cast<uint32_t>(methTv.m_value >> 32) };
    TValue fnName = TValue::Create<tObject>(TranslateToHeapPtr(vm->GetInternedSymbol(meth.m_id)));
    SOMObject* args = SOMObject::AllocateArray(numArgs);
    for (size_t i = 0; i < numArgs; i++)
    {
        args->m_data[i + 1] = argStart[i];
    }
    MakeCall(handler.As(), self, fnName, TValue::Create<tObject>(TranslateToHeapPtr(args)), RetCont);
}
