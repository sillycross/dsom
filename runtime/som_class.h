#pragma once

#include "common_utils.h"

#include "heap_object_common.h"
#include "memory_ptr.h"
#include "string_interner.h"
#include "tvalue.h"

class SOMObject;

struct SOMUniquedString
{
    uint32_t m_id;
    uint32_t m_hash;
};

// Takes bit [0:4) of the m_arrayType field
//
enum SOMDetailEntityType : uint8_t
{
    SOM_Object,
    SOM_Method,
    SOM_BlockNoArg,     // excluding self
    // The 'ImmSelf' version means the 'self' is captured as an immutable upvalue
    //
    SOM_BlockNoArgImmSelf,
    SOM_BlockOneArg,
    SOM_BlockOneArgImmSelf,
    SOM_BlockTwoArgs,
    SOM_BlockTwoArgsImmSelf,
    SOM_Array,
    // This is either a string or a symbol
    //
    SOM_String
};

// Takes bit [4:8) of the m_arrayType field
//
enum SOMMethodLookupResultKind : uint8_t
{
    SOM_MethodNotFound,
    SOM_CallBaseNotObject,
    SOM_NormalMethod,
    SOM_LiteralReturn,
    SOM_GlobalReturn,
    SOM_Getter,
    SOM_Setter
};

class SOMObject : public UserHeapGcObjectHeader
{
public:
    static SOMObject* WARN_UNUSED AllocateUninitialized(size_t trailingArraySize);

    static SOMObject* WARN_UNUSED AllocateString(std::string_view str);
    static SOMObject* WARN_UNUSED AllocateArray(size_t length);
    SOMObject* WARN_UNUSED ShallowCopyArray();

    static HeapPtr<SOMObject> WARN_UNUSED DoStringConcat(HeapPtr<SOMObject> lhs, HeapPtr<SOMObject> rhs);

    TValue m_data[0];
};

class SOMClass : public SystemHeapGcObjectHeader
{
public:
    struct MethodHtEntry
    {
        uint32_t m_id;
        GeneralHeapPointer<FunctionObject> m_data;
    };

    static GeneralHeapPointer<FunctionObject> WARN_UNUSED GetMethod(HeapPtr<SOMClass> self, SOMUniquedString name)
    {
        size_t slot1 = name.m_hash & self->m_methodHtMask;
        size_t slot2 = (name.m_hash >> 16) & self->m_methodHtMask;
        if (likely(self->m_methodHt[slot1].m_id == name.m_id)) return TCGet(self->m_methodHt[slot1].m_data);
        if (likely(self->m_methodHt[slot2].m_id == name.m_id)) return TCGet(self->m_methodHt[slot2].m_data);
        return GeneralHeapPointer<FunctionObject>{};
    }

    // Does not initialize anything, only allocate the uninitialized memory
    //
    static SOMClass* WARN_UNUSED AllocateUnintialized(size_t methodHtSize);

    static HeapPtr<SOMClass> WARN_UNUSED AllocateUninitializedSystemClass()
    {
        return TranslateToHeapPtr(AllocateUnintialized(x_bootstrapClassesMethodHtSize));
    }

    struct MethInfo
    {
        uint32_t m_stringId;
        bool m_isPrimitive;
        FunctionObject* m_fnObj;
    };

    // Does not initialize m_classObject.
    // Does not initialize the 'holder' field in the method array
    //
    static SOMClass* WARN_UNUSED Create(StringInterner* interner,
                                        std::string_view className,
                                        SOMClass* superClass,
                                        std::span<MethInfo> methods,
                                        std::span<uint32_t /*stringId*/> fields,
                                        SOMClass* preallocatedAddr = nullptr);

    static constexpr size_t x_bootstrapClassesMethodHtSize = 128;

    SOMObject* WARN_UNUSED Instantiate();

    SOMClass* m_superClass;
    // The singleton "class object" for this class
    //
    SOMObject* m_classObject;

    SOMObject* m_name;

    SOMObject* m_fields;

    // An array holding the methods
    // Each element is an object with 3 fields: field 0 = holder, field 1 = signature string, field 2 = function object
    //
    SOMObject* m_methods;

    uint32_t m_numFields;

    uint16_t m_methodHtMask;

    MethodHtEntry m_methodHt[0];
};
