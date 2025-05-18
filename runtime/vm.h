#pragma once

#include "memory_ptr.h"
#include "tvalue.h"
#include "jit_memory_allocator.h"
#include "string_interner.h"
#include "som_primitives_container.h"
#include "som_class.h"

// Uncomment to count how many times each method is called
//
//#define ENABLE_SOM_PROFILE_FREQUENCY

class SOMObject;

// Normally for each class type, we use one free list for compiler thread and one free list for execution thread.
// However, some classes may be allocated on the compiler thread but freed on the execution thread.
// If that is the case, we should use a lockfree freelist to make sure the freelist is effective
// (since otherwise a lot of freed objects would be on the execution thread free list but the compiler thread cannot grab them).
//
#define SPDS_ALLOCATABLE_CLASS_LIST             \
  /* C++ class name   Use lockfree freelist */  \
    (JitCallInlineCacheEntry,       false)      \
  , (JitGenericInlineCacheEntry,    false)

#define SPDS_CPP_NAME(e) PP_TUPLE_GET_1(e)
#define SPDS_USE_LOCKFREE_FREELIST(e) PP_TUPLE_GET_2(e)

#define macro(e) class SPDS_CPP_NAME(e);
PP_FOR_EACH(macro, SPDS_ALLOCATABLE_CLASS_LIST)
#undef macro

#define macro(e) + ((SPDS_USE_LOCKFREE_FREELIST(e)) ? 1 : 0)
constexpr size_t x_numSpdsAllocatableClassUsingLfFreelist = PP_FOR_EACH(macro, SPDS_ALLOCATABLE_CLASS_LIST);
#undef macro

#define macro(e) + ((SPDS_USE_LOCKFREE_FREELIST(e)) ? 0 : 1)
constexpr size_t x_numSpdsAllocatableClassNotUsingLfFreelist = PP_FOR_EACH(macro, SPDS_ALLOCATABLE_CLASS_LIST);
#undef macro

constexpr size_t x_numSpdsAllocatableClasses = x_numSpdsAllocatableClassUsingLfFreelist + x_numSpdsAllocatableClassNotUsingLfFreelist;

namespace internal
{

template<typename T> struct is_spds_allocatable_class : std::false_type { };

#define macro(e) template<> struct is_spds_allocatable_class<SPDS_CPP_NAME(e)> : std::true_type { };
PP_FOR_EACH(macro, SPDS_ALLOCATABLE_CLASS_LIST)
#undef macro

template<typename T> struct spds_use_lockfree_freelist { };

#define macro(e) template<> struct spds_use_lockfree_freelist<SPDS_CPP_NAME(e)> : std::integral_constant<bool, SPDS_USE_LOCKFREE_FREELIST(e)> { };
PP_FOR_EACH(macro, SPDS_ALLOCATABLE_CLASS_LIST)
#undef macro

constexpr bool x_spds_class_use_lf_freelist_by_enumid[] = {
#define macro(e) SPDS_USE_LOCKFREE_FREELIST(e),
PP_FOR_EACH(macro, SPDS_ALLOCATABLE_CLASS_LIST)
#undef macro
    false
};

// <useLFFreelist, ord> uniquely identifies the class
//
template<typename T> struct spds_class_ord { };

#define macro(ord, e)                                                                                           \
    template<> struct spds_class_ord<SPDS_CPP_NAME(e)> {                                                        \
        constexpr static uint32_t GetOrd() {                                                                    \
            uint32_t ret = 0;                                                                                   \
            for (uint32_t i = 0; i < ord; i++) {                                                                \
                if (x_spds_class_use_lf_freelist_by_enumid[i] == x_spds_class_use_lf_freelist_by_enumid[ord]) { \
                    ret++;                                                                                      \
                }                                                                                               \
            }                                                                                                   \
            return ret;                                                                                         \
        }                                                                                                       \
    };
PP_FOR_EACH_UNPACK_TUPLE(macro, PP_ZIP_TWO_LISTS((PP_NATURAL_NUMBERS_LIST), (SPDS_ALLOCATABLE_CLASS_LIST)))
#undef macro

}   // namespace internal

template<typename T>
constexpr bool x_isSpdsAllocatableClass = internal::is_spds_allocatable_class<T>::value;

template<typename T>
constexpr bool x_spdsAllocatableClassUseLfFreelist = internal::spds_use_lockfree_freelist<T>::value;

// Those classes that use LfFreelist and those do not have a different set of ordinals
//
template<typename T>
constexpr uint32_t x_spdsAllocatableClassOrdinal = internal::spds_class_ord<T>::GetOrd();

constexpr size_t x_spdsAllocationPageSize = 4096;
static_assert(is_power_of_2(x_spdsAllocationPageSize));

// If 'isTempAlloc' is true, we give the memory pages back to VM when the struct is destructed
// Singlethread use only.
// Currently chunk size is always 4KB, so allocate small objects only.
//
template<typename Host, bool isTempAlloc>
class SpdsAllocImpl
{
    MAKE_NONCOPYABLE(SpdsAllocImpl);
    MAKE_NONMOVABLE(SpdsAllocImpl);

public:
    SpdsAllocImpl(Host* host)
        : m_host(host)
        , m_curChunk(0)
        , m_lastChunkInTheChain(0)
    { }

    SpdsAllocImpl()
        : SpdsAllocImpl(nullptr)
    { }

    ~SpdsAllocImpl()
    {
        if constexpr(isTempAlloc)
        {
            ReturnMemory();
        }
    }

    void SetHost(Host* host)
    {
        m_host = host;
    }

    // If collectedByFreeList == true, we allocate at least 4 bytes to make sure it can hold a free list pointer
    //
    template<typename T, bool collectedByFreeList = false>
    SpdsPtr<T> ALWAYS_INLINE WARN_UNUSED Alloc()
    {
        static_assert(sizeof(T) <= x_spdsAllocationPageSize - RoundUpToMultipleOf<alignof(T)>(isTempAlloc ? 4ULL : 0ULL));
        constexpr uint32_t objectSize = static_cast<uint32_t>(sizeof(T));
        constexpr uint32_t allocationSize = collectedByFreeList ? std::max(objectSize, 4U) : objectSize;
        return SpdsPtr<T> { AllocMemory<alignof(T)>(allocationSize) };
    }

private:
    template<size_t alignment>
    int32_t ALWAYS_INLINE WARN_UNUSED AllocMemory(uint32_t length)
    {
        static_assert(is_power_of_2(alignment) && alignment <= 32);
        Assert(m_curChunk <= 0 && length > 0 && length % alignment == 0);

        m_curChunk &= ~static_cast<int>(alignment - 1);
        Assert(m_curChunk % static_cast<int32_t>(alignment) == 0);
        if (likely((static_cast<uint32_t>(m_curChunk) & (x_spdsAllocationPageSize - 1)) >= length))
        {
            m_curChunk -= length;
            return m_curChunk;
        }
        else
        {
            int32_t oldChunk = (m_curChunk & (~static_cast<int>(x_spdsAllocationPageSize - 1))) + static_cast<int>(x_spdsAllocationPageSize);
            m_curChunk = m_host->SpdsAllocatePage();
            Assert(m_curChunk <= 0);
            if (oldChunk > 0)
            {
                m_lastChunkInTheChain = m_curChunk;
            }
            Assert(m_curChunk % static_cast<int32_t>(x_spdsAllocationPageSize) == 0);
            if constexpr(isTempAlloc)
            {
                m_curChunk -= 4;
                *reinterpret_cast<int32_t*>(reinterpret_cast<intptr_t>(m_host) + m_curChunk) = oldChunk;
                m_curChunk &= ~static_cast<int>(alignment - 1);
            }
            m_curChunk -= length;
            return m_curChunk;
        }
    }

    void ReturnMemory()
    {
        int32_t chunk = (m_curChunk & (~static_cast<int>(x_spdsAllocationPageSize - 1))) + static_cast<int>(x_spdsAllocationPageSize);
        if (chunk != static_cast<int>(x_spdsAllocationPageSize))
        {
            m_host->SpdsPutAllocatedPagesToFreeList(chunk, m_lastChunkInTheChain);
        }
    }

    Host* m_host;
    int32_t m_curChunk;
    int32_t m_lastChunkInTheChain;
};

class ScriptModule;

class SOMClass;

// [ 12GB user heap ] [ 2GB padding ] [ 2GB short-pointer data structures ] [ 2GB system heap ]
//                                                                          ^
//     userheap                                   SPDS region     32GB aligned baseptr   systemheap
//
class VM
{
public:
    static VM* WARN_UNUSED Create();
    void Destroy();

    static VM* GetActiveVMForCurrentThread()
    {
        return reinterpret_cast<VM*>(reinterpret_cast<HeapPtr<VM>>(0)->m_self);
    }

    HeapPtrTranslator GetHeapPtrTranslator() const
    {
        return HeapPtrTranslator { VMBaseAddress() };
    }

    void SetUpSegmentationRegister()
    {
        X64_SetSegmentationRegister<X64SegmentationRegisterKind::GS>(VMBaseAddress());
    }

    SpdsAllocImpl<VM, true /*isTempAlloc*/> WARN_UNUSED CreateSpdsArenaAlloc()
    {
        return SpdsAllocImpl<VM, true /*isTempAlloc*/>(this);
    }

    SpdsAllocImpl<VM, false /*isTempAlloc*/>& WARN_UNUSED GetExecutionThreadSpdsAlloc()
    {
        return m_executionThreadSpdsAlloc;
    }

    SpdsAllocImpl<VM, false /*isTempAlloc*/>& WARN_UNUSED GetSpdsAllocForCurrentThread()
    {
        return GetExecutionThreadSpdsAlloc();
    }

    // Grab one 4K page from the SPDS region.
    // The page can be given back via SpdsPutAllocatedPagesToFreeList()
    // NOTE: if the page is [begin, end), this returns 'end', not 'begin'! SpdsReturnMemoryFreeList also expects 'end', not 'begin'
    //
    int32_t WARN_UNUSED ALWAYS_INLINE SpdsAllocatePage()
    {
        {
            int32_t out;
            if (SpdsAllocateTryGetFreeListPage(&out))
            {
                return out;
            }
        }
        return SpdsAllocatePageSlowPath();
    }

    void SpdsPutAllocatedPagesToFreeList(int32_t firstPage, int32_t lastPage)
    {
        std::atomic<int32_t>* addr = reinterpret_cast<std::atomic<int32_t>*>(VMBaseAddress() + SignExtendTo<uint64_t>(lastPage) - 4);
        uint64_t taggedValue = m_spdsPageFreeList.load(std::memory_order_relaxed);
        while (true)
        {
            uint32_t tag = BitwiseTruncateTo<uint32_t>(taggedValue >> 32);
            int32_t head = BitwiseTruncateTo<int32_t>(taggedValue);
            addr->store(head, std::memory_order_relaxed);

            tag++;
            uint64_t newTaggedValue = (static_cast<uint64_t>(tag) << 32) | ZeroExtendTo<uint64_t>(firstPage);
            if (m_spdsPageFreeList.compare_exchange_weak(taggedValue /*expected, inout*/, newTaggedValue /*desired*/, std::memory_order_release, std::memory_order_relaxed))
            {
                break;
            }
        }
    }

    // Allocate a chunk of memory from the user heap
    // Only execution thread may do this
    //
    UserHeapPointer<void> WARN_UNUSED AllocFromUserHeap(uint32_t length)
    {
        Assert(length > 0 && length % 8 == 0);
        // TODO: we currently do not have GC, so it's only a bump allocator..
        //
        m_userHeapCurPtr -= static_cast<int64_t>(length);
        if (unlikely(m_userHeapCurPtr < m_userHeapPtrLimit))
        {
            BumpUserHeap();
        }
        return UserHeapPointer<void> { reinterpret_cast<HeapPtr<void>>(m_userHeapCurPtr) };
    }

    // Allocate a chunk of memory from the system heap
    // Only execution thread may do this
    //
    SystemHeapPointer<void> WARN_UNUSED AllocFromSystemHeap(uint32_t length)
    {
        Assert(length > 0 && length % 8 == 0);
        // TODO: we currently do not have GC, so it's only a bump allocator..
        //
        uint32_t result = m_systemHeapCurPtr;
        VM_FAIL_IF(AddWithOverflowCheck(m_systemHeapCurPtr, length, &m_systemHeapCurPtr),
            "Resource limit exceeded: system heap overflowed 4GB memory limit.");

        if (unlikely(m_systemHeapCurPtr > m_systemHeapPtrLimit))
        {
            BumpSystemHeap();
        }
        return SystemHeapPointer<void> { result };
    }

    // Note: the memory returned matches the alignment and size of T, but is NOT initialized! One must call constructor of T manually.
    //
    template<typename T>
    T* WARN_UNUSED AllocateFromSpdsRegionUninitialized()
    {
        static_assert(x_isSpdsAllocatableClass<T>, "T is not registered as a SPDS allocatable class!");
        static_assert(!x_spdsAllocatableClassUseLfFreelist<T>, "unimplemented yet");
        if constexpr(!x_spdsAllocatableClassUseLfFreelist<T>)
        {
            SpdsPtr<void>& freelist = m_spdsExecutionThreadFreeList[x_spdsAllocatableClassOrdinal<T>];
            if (likely(freelist.m_value != 0))
            {
                HeapPtr<void> result = freelist.AsPtr();
                freelist = TCGet(*reinterpret_cast<HeapPtr<SpdsPtr<void>>>(result));
                return GetHeapPtrTranslator().TranslateToRawPtr<T>(reinterpret_cast<HeapPtr<T>>(result));
            }
            else
            {
                SpdsPtr<T> result = GetSpdsAllocForCurrentThread().template Alloc<T, true /*collectedByFreeList*/>();
                return GetHeapPtrTranslator().TranslateToRawPtr<T>(result.AsPtr());
            }
        }
        else
        {
            ReleaseAssert(false);
        }
    }

    // Deallocate an object returned by AllocateFromSpdsRegionUninitialized
    // Call destructor and put back to free list
    //
    template<typename T, bool callDestructor = true>
    void DeallocateSpdsRegionObject(T* object)
    {
        static_assert(x_isSpdsAllocatableClass<T>, "T is not registered as a SPDS allocatable class!");
        static_assert(!x_spdsAllocatableClassUseLfFreelist<T>, "unimplemented yet");
        if (callDestructor)
        {
            object->~T();
        }
        if constexpr(!x_spdsAllocatableClassUseLfFreelist<T>)
        {
            SpdsPtr<void>& freelist = m_spdsExecutionThreadFreeList[x_spdsAllocatableClassOrdinal<T>];

            UnalignedStore<int32_t>(object, freelist.m_value);
            freelist = GetHeapPtrTranslator().TranslateToSpdsPtr<void>(object);
        }
        else
        {
            ReleaseAssert(false);
        }
    }

    FILE* WARN_UNUSED GetStdout() { return m_filePointerForStdout; }
    FILE* WARN_UNUSED GetStderr() { return m_filePointerForStderr; }

    void RedirectStdout(FILE* newStdout) { m_filePointerForStdout = newStdout; }
    void RedirectStderr(FILE* newStderr) { m_filePointerForStderr = newStderr; }

    CoroutineRuntimeContext* GetRootCoroutine()
    {
        return m_rootCoroutine;
    }

    static CoroutineRuntimeContext* VM_GetRootCoroutine()
    {
        constexpr size_t offset = offsetof_member_v<&VM::m_rootCoroutine>;
        using T = typeof_member_t<&VM::m_rootCoroutine>;
        return *reinterpret_cast<HeapPtr<T>>(offset);
    }

    HeapPtr<void> GetRootGlobalObject() { return nullptr; }

    std::pair<TValue* /*retStart*/, uint64_t /*numRet*/> LaunchScript(ScriptModule* module);

    // Determines the starting tier of the Lua functions when a new CodeBlock is created
    // (which happens either when a Lua chunk is parsed, or when an existing Lua chunk is
    // instantiated with an unseen global object)
    //
    // When 'BaselineJIT' is selected, any newly-created CodeBlock will be compiled to baseline JIT code immediately.
    //
    enum class EngineStartingTier
    {
        Interpreter,
        BaselineJIT
    };

    // Only affects CodeBlocks created after this call.
    //
    void SetEngineStartingTier(EngineStartingTier tier)
    {
        m_isEngineStartingTierBaselineJit = (tier == EngineStartingTier::BaselineJIT);
    }

    EngineStartingTier GetEngineStartingTier() const
    {
        return m_isEngineStartingTierBaselineJit ? EngineStartingTier::BaselineJIT : EngineStartingTier::Interpreter;
    }

    bool IsEngineStartingTierBaselineJit() const
    {
        return GetEngineStartingTier() == EngineStartingTier::BaselineJIT;
    }

    // Must be ordered from lower tier to higher tier
    //
    enum class EngineMaxTier : uint8_t
    {
        Interpreter,
        BaselineJIT,
        Unrestricted    // same effect as specifying the last tier of the above list
    };

    // Only affects CodeBlocks created or tiered-up after this call.
    //
    void SetEngineMaxTier(EngineMaxTier tier) { m_engineMaxTier = tier; }

    // Return true if interpreter may tier up to a higher tier
    //
    bool WARN_UNUSED InterpreterCanTierUpFurther() { return m_engineMaxTier > EngineMaxTier::Interpreter; }

    // Return true if baseline JIT may tier up to a higher tier
    //
    bool WARN_UNUSED BaselineJitCanTierUpFurther() { return false; }

    JitMemoryAllocator* GetJITMemoryAlloc()
    {
        return &m_jitMemoryAllocator;
    }

    uint32_t GetNumTotalBaselineJitCompilations() { return m_totalBaselineJitCompilations; }
    void IncrementNumTotalBaselineJitCompilations() { m_totalBaselineJitCompilations++; }

    SOMObject* GetInternedString(size_t ord);
    SOMObject* GetInternedSymbol(size_t ord);

    static constexpr size_t x_pageSize = 4096;

private:
    static constexpr size_t x_vmLayoutLength = 18ULL << 30;
    // The start address of the VM is always at 16GB % 32GB, this makes sure the VM base is aligned at 32GB
    //
    static constexpr size_t x_vmLayoutAlignment = 32ULL << 30;
    static constexpr size_t x_vmLayoutAlignmentOffset = 16ULL << 30;
    static constexpr size_t x_vmBaseOffset = 16ULL << 30;
    static constexpr size_t x_vmUserHeapSize = 12ULL << 30;

    static_assert((1ULL << x_vmBasePtrLog2Alignment) == x_vmLayoutAlignment, "the constants must match");

    uintptr_t VMBaseAddress() const
    {
        uintptr_t result = reinterpret_cast<uintptr_t>(this);
        Assert(result == m_self);
        return result;
    }

    void __attribute__((__preserve_most__)) BumpUserHeap();
    void BumpSystemHeap();

    bool WARN_UNUSED SpdsAllocateTryGetFreeListPage(int32_t* out)
    {
        uint64_t taggedValue = m_spdsPageFreeList.load(std::memory_order_acquire);
        while (true)
        {
            int32_t head = BitwiseTruncateTo<int32_t>(taggedValue);
            Assert(head % static_cast<int32_t>(x_spdsAllocationPageSize) == 0);
            if (head == x_spdsAllocationPageSize)
            {
                return false;
            }
            uint32_t tag = BitwiseTruncateTo<uint32_t>(taggedValue >> 32);

            std::atomic<int32_t>* addr = reinterpret_cast<std::atomic<int32_t>*>(VMBaseAddress() + SignExtendTo<uint64_t>(head) - 4);
            int32_t newHead = addr->load(std::memory_order_relaxed);
            Assert(newHead % static_cast<int32_t>(x_spdsAllocationPageSize) == 0);
            tag++;
            uint64_t newTaggedValue = (static_cast<uint64_t>(tag) << 32) | ZeroExtendTo<uint64_t>(newHead);

            if (m_spdsPageFreeList.compare_exchange_weak(taggedValue /*expected, inout*/, newTaggedValue /*desired*/, std::memory_order_release, std::memory_order_acquire))
            {
                *out = head;
                return true;
            }
        }
    }

    int32_t WARN_UNUSED SpdsAllocatePageSlowPath();

    // Allocate a chunk of memory, return one of the pages, and put the rest into free list
    //
    int32_t WARN_UNUSED SpdsAllocatePageSlowPathImpl();

    bool WARN_UNUSED InitializeVMBase();
    bool WARN_UNUSED InitializeVMGlobalData();
    bool WARN_UNUSED Initialize();
    void Cleanup();
    void CreateRootCoroutine();

    // The data members
    //

    // must be first member, stores the value of static_cast<CRTP*>(this)
    //
    uintptr_t m_self;

    bool m_isEngineStartingTierBaselineJit;
    EngineMaxTier m_engineMaxTier;

    alignas(64) SpdsAllocImpl<VM, false /*isTempAlloc*/> m_executionThreadSpdsAlloc;

    // user heap region grows from high address to low address
    // highest physically mapped address of the user heap region (offsets from m_self)
    //
    int64_t m_userHeapPtrLimit;

    // lowest logically used address of the user heap region (offsets from m_self)
    //
    int64_t m_userHeapCurPtr;

    // system heap region grows from low address to high address
    // lowest physically unmapped address of the system heap region (offsets from m_self)
    //
    uint32_t m_systemHeapPtrLimit;

    // lowest logically available address of the system heap region (offsets from m_self)
    //
    uint32_t m_systemHeapCurPtr;

    SpdsPtr<void> m_spdsExecutionThreadFreeList[x_numSpdsAllocatableClassNotUsingLfFreelist];

    JitMemoryAllocator m_jitMemoryAllocator;

    uint32_t m_totalBaselineJitCompilations;

    alignas(64) std::mutex m_spdsAllocationMutex;

    // SPDS region grows from high address to low address
    //
    std::atomic<uint64_t> m_spdsPageFreeList;
    int32_t m_spdsPageAllocLimit;

    CoroutineRuntimeContext* m_rootCoroutine;

    // Allow unit test to hook stdout and stderr to a custom temporary file
    //
    FILE* m_filePointerForStdout;
    FILE* m_filePointerForStderr;

public:
    StringInterner m_interner;
    std::vector<SOMObject*> m_internedStringObjects;
    std::vector<SOMObject*> m_internedSymbolObjects;
    std::unordered_map<size_t, SOMClass*> m_parsedClasses;
    std::unordered_map<size_t /*internStringOrd*/, size_t /*idx*/> m_globalIdxMap;
    std::vector<size_t> m_globalStringIdWithIndex;
    std::vector<TValue> m_globalsVec;
    TValue* m_somGlobals;

    bool WARN_UNUSED GetSlotForGlobalNoCreate(std::string_view globalName, size_t& res /*out*/)
    {
        size_t ord = m_interner.InternString(globalName);
        auto it = m_globalIdxMap.find(ord);
        if (it == m_globalIdxMap.end()) { return false; }
        res = it->second;
        return true;
    }

    // Creates a slot if not exist
    //
    size_t WARN_UNUSED GetSlotForGlobal(std::string_view globalName)
    {
        size_t ord = m_interner.InternString(globalName);
        auto it = m_globalIdxMap.find(ord);
        if (it == m_globalIdxMap.end())
        {
            m_globalsVec.push_back(TValue::CreateImpossibleValue());
            m_somGlobals = m_globalsVec.data();
            m_globalIdxMap[ord] = m_globalsVec.size() - 1;
            m_globalStringIdWithIndex.push_back(ord);
            return m_globalsVec.size() - 1;
        }
        else
        {
            return it->second;
        }
    }

    static TValue VM_GetGlobal(uint16_t idx)
    {
        TestAssert(idx < VM_GetActiveVMForCurrentThread()->m_globalsVec.size());
        TestAssert(VM_GetActiveVMForCurrentThread()->m_globalsVec.data() == VM_GetActiveVMForCurrentThread()->m_somGlobals);
        TValue* vec = *reinterpret_cast<HeapPtr<TValue*>>(offsetof_member_v<&VM::m_somGlobals>);
        return vec[idx];
    }

    // Hidden classes for various system classes, non-boxed types and function types
    //
    HeapPtr<SOMClass> m_stringHiddenClass;
    HeapPtr<SOMClass> m_arrayHiddenClass;
    HeapPtr<SOMClass> m_objectClass;
    HeapPtr<SOMClass> m_classClass;
    HeapPtr<SOMClass> m_metaclassClass;
    HeapPtr<SOMClass> m_nilClass;
    HeapPtr<SOMClass> m_booleanClass;
    HeapPtr<SOMClass> m_trueClass;
    HeapPtr<SOMClass> m_falseClass;
    HeapPtr<SOMClass> m_integerClass;
    HeapPtr<SOMClass> m_doubleClass;
    HeapPtr<SOMClass> m_blockClass;
    HeapPtr<SOMClass> m_block1Class;
    HeapPtr<SOMClass> m_block2Class;
    HeapPtr<SOMClass> m_block3Class;
    HeapPtr<SOMClass> m_methodClass;
    HeapPtr<SOMClass> m_symbolClass;
    HeapPtr<SOMClass> m_primitiveClass;
    HeapPtr<SOMClass> m_systemClass;

    bool m_metaclassClassLoaded;

    SOMUniquedString m_unknownGlobalHandler;
    SOMUniquedString m_doesNotUnderstandHandler;
    SOMUniquedString m_escapedBlockHandler;

    SOMPrimitivesContainer m_somPrimitives;

    size_t m_stringIdForWhileTrue;
    size_t m_stringIdForWhileFalse;
    size_t m_stringIdForIfTrueIfFalse;
    size_t m_stringIdForIfFalseIfTrue;
    size_t m_stringIdForIfNilIfNotNil;
    size_t m_stringIdForIfNotNilIfNil;
    size_t m_stringIdForIfTrue;
    size_t m_stringIdForIfFalse;
    size_t m_stringIdForIfNil;
    size_t m_stringIdForIfNotNil;
    size_t m_stringIdForOperatorAnd;
    size_t m_stringIdForOperatorOr;
    size_t m_stringIdForMethodAnd;
    size_t m_stringIdForMethodOr;
    size_t m_stringIdForToDo;
    size_t m_stringIdForDowntoDo;

    PerfTimer m_vmStartTime;

#ifdef ENABLE_SOM_PROFILE_FREQUENCY
    size_t WARN_UNUSED GetMethodIndexForFrequencyProfiling(std::string_view className, std::string_view methName, bool isClassSide)
    {
        size_t& it = m_methCallCountIdxMap[static_cast<size_t>(isClassSide)][std::string(className)][std::string(methName)];
        if (it == 0)
        {
            m_methCallCounts.push_back(0);
            m_methCallCountArr = m_methCallCounts.data();
            it = m_methCallCounts.size();
        }
        TestAssert(1 <= it && it <= m_methCallCounts.size());
        return it - 1;
    }

    void NO_INLINE IncrementPrimitiveFuncCallCount(std::string_view str)
    {
        m_primMethCounts[std::string(str)]++;
    }

    void PrintSOMFunctionFrequencyProfile()
    {
        std::vector<std::pair<int64_t /*negCount*/, std::string /*name*/>> allEntries;
        std::vector<bool> idxShowedUp;
        idxShowedUp.resize(m_methCallCounts.size(), false);
        for (bool isClassSide : { false, true })
        {
            for (auto& classIt : m_methCallCountIdxMap[static_cast<size_t>(isClassSide)])
            {
                std::string_view className = classIt.first;
                for (auto& methIt : classIt.second)
                {
                    std::string_view methName = methIt.first;
                    size_t idx = methIt.second - 1;
                    TestAssert(idx < m_methCallCounts.size());
                    TestAssert(!idxShowedUp[idx]);
                    idxShowedUp[idx] = true;
                    size_t count = m_methCallCounts[idx];
                    if (count > 0)
                    {
                        std::string desc = std::string(className) + "." + std::string(methName);
                        if (isClassSide) { desc += " [ClassSide]"; }
                        allEntries.push_back(std::make_pair(-static_cast<int64_t>(count), desc));
                    }
                }
            }
        }
        for (bool value : idxShowedUp) { TestAssert(value); std::ignore = value; }

        for (auto& it : m_primMethCounts)
        {
            if (it.second > 0)
            {
                allEntries.push_back(std::make_pair(-static_cast<int64_t>(it.second), std::string(it.first) + " [Prim]"));
            }
        }

        std::sort(allEntries.begin(), allEntries.end());

        size_t totalCnts = 0;
        for (auto& it : allEntries) { totalCnts += static_cast<size_t>(-it.first); }

        fprintf(stderr, "========= SOM function frequency stats =========\n");
        fprintf(stderr, "    Freq Name\n");
        size_t printedCnts = 0;
        size_t numToPrint = 30;
        numToPrint = std::min(numToPrint, allEntries.size());
        for (size_t i = 0; i < numToPrint; i++)
        {
            size_t cnt = static_cast<size_t>(-allEntries[i].first);
            printedCnts += cnt;
            fprintf(stderr, "%8llu %s\n", static_cast<unsigned long long>(cnt), allEntries[i].second.c_str());
        }
        fprintf(stderr, "Long tail: %llu functions accounting for %.1lf%% (%llu) of all calls (%llu) omitted.\n",
                static_cast<unsigned long long>(allEntries.size() - numToPrint),
                100.0 * static_cast<double>(totalCnts - printedCnts) / static_cast<double>(totalCnts),
                static_cast<unsigned long long>(totalCnts - printedCnts),
                static_cast<unsigned long long>(totalCnts));
    }

    std::unordered_map<std::string /*className*/, std::unordered_map<std::string /*methName*/, size_t /*idx+1*/>> m_methCallCountIdxMap[2];
    std::vector<size_t> m_methCallCounts;
    size_t* m_methCallCountArr;
    std::unordered_map<std::string, size_t /*count*/> m_primMethCounts;
#endif  // ifdef ENABLE_SOM_PROFILE_FREQUENCY
};

