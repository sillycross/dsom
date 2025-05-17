#include "som_class.h"
#include "temp_arena_allocator.h"
#include "string_interner.h"
#include "vm.h"
#include "runtime_utils.h"

struct SimpleCuckooHashTable
{
    using Entry = SOMClass::MethodHtEntry;

    SimpleCuckooHashTable(TempArenaAllocator& alloc, StringInterner* interner)
        : m_interner(interner), m_data(alloc)
    {
        m_data.resize(1);
        m_data[0].m_id = static_cast<uint32_t>(-1);
        m_data[0].m_data = GeneralHeapPointer<FunctionObject>{};
    }

    enum class InsertionResult
    {
        Inserted,
        Found,
        Failed
    };

    void CloneFrom(std::span<Entry> src)
    {
        TestAssert(is_power_of_2(src.size()));
        m_data.resize(src.size());
        memcpy(m_data.data(), src.data(), sizeof(Entry) * src.size());
    }

    void GetHash(uint32_t key, size_t& slot1 /*out*/, size_t& slot2 /*out*/)
    {
        uint64_t hash64 = m_interner->GetHash(key);
        slot1 = hash64 & (m_data.size() - 1);
        slot2 = (hash64 >> 16) & (m_data.size() - 1);
    }

    // If failed, 'key/value' is updated to the kv pair that gets evicted out of the table
    //
    InsertionResult WARN_UNUSED TryInsert(uint32_t& key, GeneralHeapPointer<FunctionObject>& value)
    {
        size_t h1, h2;
        GetHash(key, h1, h2);
        if (m_data[h1].m_id == key)
        {
            m_data[h1].m_data = value;
            return InsertionResult::Found;
        }
        if (m_data[h2].m_id == key)
        {
            m_data[h2].m_data = value;
            return InsertionResult::Found;
        }
        if (m_data[h1].m_id == static_cast<uint32_t>(-1))
        {
            m_data[h1].m_id = key;
            m_data[h1].m_data = value;
            return InsertionResult::Inserted;
        }
        if (m_data[h2].m_id == static_cast<uint32_t>(-1))
        {
            m_data[h2].m_id = key;
            m_data[h2].m_data = value;
            return InsertionResult::Inserted;
        }
        size_t victimPosition = h1;
        size_t rounds = 1;
        while (true)
        {
            uint32_t victimKey = m_data[victimPosition].m_id;
            GeneralHeapPointer<FunctionObject> victimValue = m_data[victimPosition].m_data;
            m_data[victimPosition].m_id = key;
            m_data[victimPosition].m_data = value;
            GetHash(victimKey, h1, h2);
            if (h1 == victimPosition)
            {
                std::swap(h1, h2);
            }
            TestAssert(h2 == victimPosition);
            if (m_data[h1].m_id == static_cast<uint32_t>(-1))
            {
                m_data[h1].m_id = victimKey;
                m_data[h1].m_data = victimValue;
                return InsertionResult::Inserted;
            }
            key = victimKey;
            value = victimValue;
            victimPosition = h1;
            rounds++;
            if (rounds > 30) { return InsertionResult::Failed; }
        }
    }

    InsertionResult Insert(uint32_t key, GeneralHeapPointer<FunctionObject> value)
    {
        [[maybe_unused]] bool resized = false;
        while (true)
        {
            InsertionResult res = TryInsert(key, value);
            TestAssertImp(resized, res == InsertionResult::Inserted);
            if (res != InsertionResult::Failed)
            {
                return res;
            }
            Resize();
            resized = true;
        }
    }

    void Resize()
    {
        ReleaseAssert(m_data.size() <= 32768);
        TempVector<Entry> t = m_data;
        m_data.resize(t.size() * 2);
        for (size_t i = 0; i < m_data.size(); i++)
        {
            m_data[i].m_id = static_cast<uint32_t>(-1);
            m_data[i].m_data = GeneralHeapPointer<FunctionObject>{};
        }
        for (size_t i = 0; i < t.size(); i++)
        {
            if (t[i].m_id != static_cast<uint32_t>(-1))
            {
                InsertionResult res = TryInsert(t[i].m_id, t[i].m_data);
                TestAssert(res == InsertionResult::Inserted);
                std::ignore = res;
            }
        }
    }

    StringInterner* m_interner;
    TempVector<Entry> m_data;
};

SOMClass* WARN_UNUSED SOMClass::AllocateUnintialized(size_t methodHtSize)
{
    TestAssert(is_power_of_2(methodHtSize));
    TestAssert(methodHtSize <= 65536);
    VM* vm = VM_GetActiveVMForCurrentThread();
    size_t allocSize = offsetof_member_v<&SOMClass::m_methodHt> + sizeof(MethodHtEntry) * methodHtSize;
    SOMClass* c = TranslateToRawPointer(vm, vm->AllocFromSystemHeap(static_cast<uint32_t>(allocSize)).AsNoAssert<SOMClass>());
    SOMClass::Populate(c);
    return c;
}

SOMClass* WARN_UNUSED SOMClass::Create(StringInterner* interner,
                                       std::string_view className,
                                       SOMClass* superClass,
                                       std::span<MethInfo> methods,
                                       std::span<uint32_t /*stringId*/> fields,
                                       SOMClass* preallocatedAddr)
{
    TempArenaAllocator alloc;
    SimpleCuckooHashTable methodHt(alloc, interner);
    if (superClass != nullptr)
    {
        methodHt.CloneFrom(std::span<MethodHtEntry> { superClass->m_methodHt, superClass->m_methodHt + superClass->m_methodHtMask + 1 });
    }
    for (auto& it : methods)
    {
        TestAssert(it.m_fnObj != nullptr);
        //fprintf(stderr, "Inserting method %s\n", std::string(VM_GetActiveVMForCurrentThread()->m_interner.Get(it.first)).c_str());
        methodHt.Insert(it.m_stringId, it.m_fnObj);
    }

    if (preallocatedAddr != nullptr)
    {
        // We rely on the fact that all system classes have a method hash table of size exactly 128.
        // If this no longer holds, we need to let the caller pass in the size explicitly for us to assert.
        //
        if (methodHt.m_data.size() != x_bootstrapClassesMethodHtSize)
        {
            fprintf(stderr, "Unexpected method hash table size for pre-allocated system class!\n");
            abort();
        }
    }

    VM* vm = VM_GetActiveVMForCurrentThread();
    SOMClass* c = preallocatedAddr;
    if (c == nullptr)
    {
        c = AllocateUnintialized(methodHt.m_data.size());
    }
    SOMClass::Populate(c);

    // In SOM if a subclass has field with same name as in superclass, it's undefined behavior, so no need to dedup.
    //
    uint32_t numFields = static_cast<uint32_t>(fields.size());
    if (superClass != nullptr) { numFields += superClass->m_numFields; }

    c->m_superClass = superClass;
    c->m_classObject = nullptr;
    c->m_name = vm->GetInternedSymbol(vm->m_interner.InternString(className));
    c->m_fields = SOMObject::AllocateArray(numFields);

    {
        TValue* curPtr = c->m_fields->m_data + 1;
        if (superClass != nullptr)
        {
            memcpy(curPtr, superClass->m_fields->m_data + 1, sizeof(TValue) * superClass->m_numFields);
            curPtr += superClass->m_numFields;
        }
        for (uint32_t stringId : fields)
        {
            SOMObject* strVal = vm->GetInternedSymbol(stringId);
            *curPtr = TValue::Create<tObject>(TranslateToHeapPtr(strVal));
            curPtr++;
        }
    }

    c->m_numFields = numFields;
    TestAssert(is_power_of_2(methodHt.m_data.size()));
    TestAssert(methodHt.m_data.size() <= 65535);
    c->m_methodHtMask = SafeIntegerCast<uint16_t>(methodHt.m_data.size() - 1);
    memcpy(c->m_methodHt, methodHt.m_data.data(), sizeof(MethodHtEntry) * methodHt.m_data.size());

    c->m_methods = SOMObject::AllocateArray(methods.size());
    for (size_t i = 0; i < methods.size(); i++)
    {
        SOMObject* mo = SOMObject::AllocateUninitialized(8 * 3);
        SOMObject::Populate(mo);
        HeapPtr<SOMClass> hc = (methods[i].m_isPrimitive ? vm->m_primitiveClass : vm->m_methodClass);
        mo->m_hiddenClass = SystemHeapPointer<SOMClass>(hc).m_value;
        mo->m_arrayType = SOM_Object;
        SOMObject* sig = vm->GetInternedSymbol(methods[i].m_stringId);
        // m_data[0] is the holder (classObject), which is populated by caller logic later. Only populate slot 1/2 here.
        mo->m_data[1] = TValue::Create<tObject>(TranslateToHeapPtr(sig));
        TestAssert(methods[i].m_fnObj != nullptr);
        mo->m_data[2] = TValue::Create<tFunction>(TranslateToHeapPtr(methods[i].m_fnObj));
        c->m_methods->m_data[i + 1] = TValue::Create<tObject>(TranslateToHeapPtr(mo));
    }

#ifdef TESTBUILD
    {
        for (auto& it : methods)
        {
            SOMUniquedString str {
                .m_id = static_cast<uint32_t>(it.m_stringId),
                .m_hash = static_cast<uint32_t>(interner->GetHash(it.m_stringId))
            };
            GeneralHeapPointer<FunctionObject> fn = SOMClass::GetMethod(TranslateToHeapPtr(c), str);
            TestAssert(fn.m_value != 0);
            TestAssert(TranslateToRawPointer(fn.As()) == it.m_fnObj);

            //fprintf(stderr, "function %s %s allocated at 0x%llx\n",
            //        std::string(className).c_str(), std::string(vm->m_interner.Get(it.first)).c_str(), static_cast<unsigned long long>(reinterpret_cast<uint64_t>(it.second)));
        }
    }
#endif

    return c;
}

SOMObject* WARN_UNUSED SOMClass::Instantiate()
{
    SOMObject* o = SOMObject::AllocateUninitialized(8 * m_numFields);
    SOMObject::Populate(o);
    o->m_hiddenClass = SystemHeapPointer<SOMClass>(this).m_value;
    o->m_arrayType = SOM_Object;
    for (size_t i = 0; i < m_numFields; i++)
    {
        o->m_data[i] = TValue::Create<tNil>();
    }
    return o;
}

SOMObject* WARN_UNUSED SOMObject::AllocateUninitialized(size_t trailingArraySize)
{
    VM* vm = VM_GetActiveVMForCurrentThread();
    size_t allocSize = RoundUpToMultipleOf<8>(offsetof_member_v<&SOMObject::m_data> + trailingArraySize);
    SOMObject* o = TranslateToRawPointer(vm, vm->AllocFromUserHeap(static_cast<uint32_t>(allocSize)).AsNoAssert<SOMObject>());
    return o;
}

SOMObject* WARN_UNUSED SOMObject::AllocateString(std::string_view str)
{
    VM* vm = VM_GetActiveVMForCurrentThread();
    SOMObject* o = AllocateUninitialized(8 + str.size() + 1);
    SOMObject::Populate(o);
    o->m_hiddenClass = SystemHeapPointer<SOMClass>(vm->m_stringHiddenClass).m_value;
    o->m_arrayType = SOM_String;
    o->m_data[0].m_value = str.size();
    memcpy(&o->m_data[1], str.data(), str.size());
    reinterpret_cast<char*>(&o->m_data[1])[str.size()] = '\0';
    return o;
}

SOMObject* WARN_UNUSED SOMObject::AllocateArray(size_t length)
{
    VM* vm = VM_GetActiveVMForCurrentThread();
    SOMObject* o = AllocateUninitialized(8 + length * 8);
    SOMObject::Populate(o);
    o->m_hiddenClass = SystemHeapPointer<SOMClass>(vm->m_arrayHiddenClass).m_value;
    o->m_arrayType = SOM_Array;
    o->m_data[0].m_value = length;
    for (size_t i = 1; i <= length; i++)
    {
        o->m_data[i] = TValue::Create<tNil>();
    }
    return o;
}

SOMObject* WARN_UNUSED SOMObject::ShallowCopyArray()
{
    TestAssert(m_arrayType == SOM_Array);
    size_t length = m_data[0].m_value;
    VM* vm = VM_GetActiveVMForCurrentThread();
    SOMObject* o = AllocateUninitialized(8 + length * 8);
    SOMObject::Populate(o);
    o->m_hiddenClass = SystemHeapPointer<SOMClass>(vm->m_arrayHiddenClass).m_value;
    o->m_arrayType = SOM_Array;
    o->m_data[0].m_value = length;
    memcpy(&o->m_data[1], &m_data[1], sizeof(TValue) * length);
    return o;
}

HeapPtr<SOMObject> WARN_UNUSED SOMObject::DoStringConcat(HeapPtr<SOMObject> lhs, HeapPtr<SOMObject> rhs)
{
    TestAssert(lhs->m_arrayType == SOM_String && rhs->m_arrayType == SOM_String);
    size_t llen = lhs->m_data[0].m_value;
    size_t rlen = rhs->m_data[0].m_value;
    VM* vm = VM_GetActiveVMForCurrentThread();
    SOMObject* o = AllocateUninitialized(8 + llen + rlen + 1);
    SOMObject::Populate(o);
    o->m_hiddenClass = SystemHeapPointer<SOMClass>(vm->m_stringHiddenClass).m_value;
    o->m_arrayType = SOM_String;
    o->m_data[0].m_value = llen + rlen;
    char* buf = reinterpret_cast<char*>(&o->m_data[1]);
    memcpy(buf, TranslateToRawPointer(vm, &lhs->m_data[1]), llen);
    memcpy(buf + llen, TranslateToRawPointer(vm, &rhs->m_data[1]), rlen);
    buf[llen + rlen] = '\0';
    return TranslateToHeapPtr(o);
}

