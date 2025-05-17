#pragma once

#include "common_utils.h"
#include "som_class.h"
#include "vm.h"
#include "runtime_utils.h"

// Slow path that returns the SOM class for any value, including unboxed values and non-SOMObject values
//
inline HeapPtr<SOMClass> WARN_UNUSED GetSOMClassOfAny(TValue tv)
{
    VM* vm = VM_GetActiveVMForCurrentThread();
    if (tv.Is<tObject>())
    {
        return SystemHeapPointer<SOMClass>(tv.As<tObject>()->m_hiddenClass).As();
    }
    if (tv.Is<tNil>())
    {
        return vm->m_nilClass;
    }
    if (tv.Is<tBool>())
    {
        if (tv.As<tBool>())
        {
            return vm->m_trueClass;
        }
        else
        {
            return vm->m_falseClass;
        }
    }
    if (tv.Is<tInt32>())
    {
        return vm->m_integerClass;
    }
    if (tv.Is<tDouble>())
    {
        return vm->m_doubleClass;
    }
    if (tv.Is<tFunction>())
    {
        HeapPtr<FunctionObject> f = tv.As<tFunction>();
        SOMDetailEntityType fnTy = static_cast<SOMDetailEntityType>(f->m_invalidArrayType & 15);
        switch (fnTy)
        {
        case SOMDetailEntityType::SOM_BlockNoArg:
        case SOMDetailEntityType::SOM_BlockNoArgImmSelf:
        {
            return vm->m_block1Class;
        }
        case SOMDetailEntityType::SOM_BlockOneArg:
        case SOMDetailEntityType::SOM_BlockOneArgImmSelf:
        {
            return vm->m_block2Class;
        }
        case SOMDetailEntityType::SOM_BlockTwoArgs:
        case SOMDetailEntityType::SOM_BlockTwoArgsImmSelf:
        {
            return vm->m_block3Class;
        }
        default:
        {
            // Method should never be exposed directly to application logic,
            // so should never reach here (the class.methods return wrapper objects for them)
            //
            TestAssert(false);
            __builtin_unreachable();
        }
        }   /*switch*/
    }
    TestAssert(false && "unexpected tvalue");
    __builtin_unreachable();
}

inline TValue NO_INLINE DeepCloneConstantArray(TValue tv)
{
    TestAssert(tv.Is<tObject>());
    VM* vm = VM_GetActiveVMForCurrentThread();
    SOMObject* o = TranslateToRawPointer(vm, tv.As<tObject>());
    TestAssert(o->m_hiddenClass == SystemHeapPointer<SOMClass>(vm->m_arrayHiddenClass).m_value);
    size_t len = o->m_data[0].m_value;
    SOMObject* r = SOMObject::AllocateArray(len);
    memcpy(&r->m_data[1], &o->m_data[1], sizeof(TValue) * len);
    for (size_t i = 1; i <= len; i++)
    {
        TValue x = r->m_data[i];
        if (x.Is<tObject>() && x.As<tObject>()->m_hiddenClass == SystemHeapPointer<SOMClass>(vm->m_arrayHiddenClass).m_value)
        {
            r->m_data[i] = DeepCloneConstantArray(x);
        }
    }
    return TValue::Create<tObject>(TranslateToHeapPtr(r));
}
