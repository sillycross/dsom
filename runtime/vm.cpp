#include "vm.h"
#include "runtime_utils.h"
#include "deegen_options.h"
#include "som_class.h"

VM* WARN_UNUSED VM::Create()
{
    constexpr size_t x_mmapLength = x_vmLayoutLength + x_vmLayoutAlignment * 2;
    void* ptrVoid = mmap(nullptr, x_mmapLength, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK_LOG_ERROR_WITH_ERRNO(ptrVoid != MAP_FAILED, "Failed to reserve VM address range, please make sure overcommit is allowed");

    // cut out the desired properly-aligned space, and unmap the remaining
    //
    {
        uintptr_t ptr = reinterpret_cast<uintptr_t>(ptrVoid);
        uintptr_t alignedPtr = RoundUpToMultipleOf<x_vmLayoutAlignment>(ptr);
        Assert(alignedPtr >= ptr && alignedPtr % x_vmLayoutAlignment == 0 && alignedPtr - ptr < x_vmLayoutAlignment);

        uintptr_t vmRangeStart = alignedPtr + x_vmLayoutAlignmentOffset;

        // If any unmap failed, log a warning, but continue execution.
        //
        if (vmRangeStart > ptr)
        {
            int r = munmap(reinterpret_cast<void*>(ptr), vmRangeStart - ptr);
            LOG_WARNING_WITH_ERRNO_IF(r != 0, "Failed to unmap unnecessary VM address range");
        }

        {
            uintptr_t vmRangeEnd = vmRangeStart + x_vmLayoutLength;
            uintptr_t originalMapEnd = ptr + x_mmapLength;
            Assert(vmRangeEnd <= originalMapEnd);
            if (originalMapEnd > vmRangeEnd)
            {
                int r = munmap(reinterpret_cast<void*>(vmRangeEnd), originalMapEnd - vmRangeEnd);
                LOG_WARNING_WITH_ERRNO_IF(r != 0, "Failed to unmap unnecessary VM address range");
            }
        }

        ptrVoid = reinterpret_cast<void*>(vmRangeStart);
    }

    Assert(reinterpret_cast<uintptr_t>(ptrVoid) % x_vmLayoutAlignment == x_vmLayoutAlignmentOffset);

    bool success = false;
    void* unmapPtrOnFailure = ptrVoid;
    size_t unmapLengthOnFailure = x_vmLayoutLength;

    Auto(
        if (!success)
        {
            int r = munmap(unmapPtrOnFailure, unmapLengthOnFailure);
            LOG_WARNING_WITH_ERRNO_IF(r != 0, "Cannot unmap VM on failure cleanup");
        }
    );

    // Map memory and initialize the VM struct
    //
    void* vmVoid = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ptrVoid) + x_vmBaseOffset);
    Assert(reinterpret_cast<uintptr_t>(vmVoid) % x_vmLayoutAlignment == 0);
    constexpr size_t sizeToMap = RoundUpToMultipleOf<x_pageSize>(sizeof(VM));
    {
        void* r = mmap(vmVoid, sizeToMap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_FIXED, -1, 0);
        CHECK_LOG_ERROR_WITH_ERRNO(r != MAP_FAILED, "Failed to allocate VM struct");
        TestAssert(vmVoid == r);
    }

    VM* vm = new (vmVoid) VM();
    Assert(vm == vmVoid);
    Auto(
        if (!success)
        {
            vm->~VM();
        }
    );

    CHECK_LOG_ERROR(vm->Initialize());
    Auto(
        if (!success)
        {
            vm->Cleanup();
        }
    );

    success = true;
    return vm;
}

void VM::Destroy()
{
    VM* ptr = this;
    ptr->Cleanup();
    ptr->~VM();

    void* unmapAddr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ptr) - x_vmBaseOffset);
    int r = munmap(unmapAddr, x_vmLayoutLength);
    LOG_WARNING_WITH_ERRNO_IF(r != 0, "Cannot unmap VM");
}

bool WARN_UNUSED VM::InitializeVMBase()
{
    // I'm not sure if there's any place where we expect the VM struct to not have a vtable,
    // but there is no reason it needs to have one any way
    //
    static_assert(!std::is_polymorphic_v<VM>, "must be not polymorphic");

    m_self = reinterpret_cast<uintptr_t>(this);

    SetUpSegmentationRegister();

    m_isEngineStartingTierBaselineJit = false;
    m_engineMaxTier = EngineMaxTier::Unrestricted;

    m_userHeapPtrLimit = -static_cast<int64_t>(x_vmBaseOffset - x_vmUserHeapSize);
    m_userHeapCurPtr = -static_cast<int64_t>(x_vmBaseOffset - x_vmUserHeapSize);

    static_assert(sizeof(VM) >= x_minimum_valid_heap_address);
    m_systemHeapPtrLimit = static_cast<uint32_t>(RoundUpToMultipleOf<x_pageSize>(sizeof(VM)));
    m_systemHeapCurPtr = sizeof(VM);

    m_spdsPageFreeList.store(static_cast<uint64_t>(x_spdsAllocationPageSize));
    m_spdsPageAllocLimit = -static_cast<int32_t>(x_pageSize);

    m_executionThreadSpdsAlloc.SetHost(this);

    for (size_t i = 0; i < x_numSpdsAllocatableClassNotUsingLfFreelist; i++)
    {
        m_spdsExecutionThreadFreeList[i] = SpdsPtr<void> { 0 };
    }

    m_totalBaselineJitCompilations = 0;

    return true;
}

void __attribute__((__preserve_most__)) VM::BumpUserHeap()
{
    Assert(m_userHeapCurPtr < m_userHeapPtrLimit);
    VM_FAIL_IF(m_userHeapCurPtr < -static_cast<intptr_t>(x_vmBaseOffset),
               "Resource limit exceeded: user heap overflowed %dGB memory limit.", static_cast<int>(x_vmUserHeapSize >> 30));

    constexpr size_t x_allocationSize = 65536;
    // TODO: consider allocating smaller sizes on the first few allocations
    //
    intptr_t newHeapLimit = m_userHeapCurPtr & (~static_cast<intptr_t>(x_allocationSize - 1));
    Assert(newHeapLimit <= m_userHeapCurPtr && newHeapLimit % static_cast<int64_t>(x_pageSize) == 0 && newHeapLimit < m_userHeapPtrLimit);
    size_t lengthToAllocate = static_cast<size_t>(m_userHeapPtrLimit - newHeapLimit);
    Assert(lengthToAllocate % x_pageSize == 0);

    uintptr_t allocAddr = VMBaseAddress() + static_cast<uint64_t>(newHeapLimit);
    void* r = mmap(reinterpret_cast<void*>(allocAddr), lengthToAllocate, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_FIXED, -1, 0);
    VM_FAIL_WITH_ERRNO_IF(r == MAP_FAILED,
                          "Out of Memory: Allocation of length %llu failed", static_cast<unsigned long long>(lengthToAllocate));
    Assert(r == reinterpret_cast<void*>(allocAddr));

    m_userHeapPtrLimit = newHeapLimit;
    Assert(m_userHeapPtrLimit <= m_userHeapCurPtr);
    Assert(m_userHeapPtrLimit >= -static_cast<intptr_t>(x_vmBaseOffset));
}

void VM::BumpSystemHeap()
{
    Assert(m_systemHeapCurPtr > m_systemHeapPtrLimit);
    constexpr uint32_t x_allocationSize = 65536;

    VM_FAIL_IF(m_systemHeapCurPtr > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) - x_allocationSize,
               "Resource limit exceeded: system heap overflowed 2GB memory limit.");

    // TODO: consider allocating smaller sizes on the first few allocations
    //
    uint32_t newHeapLimit = RoundUpToMultipleOf<x_allocationSize>(m_systemHeapCurPtr);
    Assert(newHeapLimit >= m_systemHeapCurPtr && newHeapLimit % static_cast<int64_t>(x_pageSize) == 0 && newHeapLimit > m_systemHeapPtrLimit);

    size_t lengthToAllocate = static_cast<size_t>(newHeapLimit - m_systemHeapPtrLimit);
    Assert(lengthToAllocate % x_pageSize == 0);

    uintptr_t allocAddr = VMBaseAddress() + static_cast<uint64_t>(m_systemHeapPtrLimit);
    void* r = mmap(reinterpret_cast<void*>(allocAddr), lengthToAllocate, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_FIXED, -1, 0);
    VM_FAIL_WITH_ERRNO_IF(r == MAP_FAILED,
                          "Out of Memory: Allocation of length %llu failed", static_cast<unsigned long long>(lengthToAllocate));
    Assert(r == reinterpret_cast<void*>(allocAddr));

    m_systemHeapPtrLimit = newHeapLimit;
    Assert(m_systemHeapPtrLimit >= m_systemHeapCurPtr);
}

int32_t WARN_UNUSED VM::SpdsAllocatePageSlowPath()
{
    while (true)
    {
        if (m_spdsAllocationMutex.try_lock())
        {
            int32_t result = SpdsAllocatePageSlowPathImpl();
            m_spdsAllocationMutex.unlock();
            return result;
        }
        else
        {
            // Someone else has took the lock, we wait until they finish, and retry
            //
            {
                std::lock_guard<std::mutex> blinkLock(m_spdsAllocationMutex);
            }
            int32_t out;
            if (SpdsAllocateTryGetFreeListPage(&out))
            {
                return out;
            }
        }
    }
}

int32_t WARN_UNUSED VM::SpdsAllocatePageSlowPathImpl()
{
    constexpr int32_t x_allocationSize = 65536;

    // Compute how much memory we should allocate
    // We allocate 4K, 8K, 16K, 32K first (the highest 4K is not used to prevent all kinds of overflowing issue)
    // After that we allocate 64K each time
    //
    Assert(m_spdsPageAllocLimit % static_cast<int32_t>(x_pageSize) == 0 && m_spdsPageAllocLimit % static_cast<int32_t>(x_spdsAllocationPageSize) == 0);
    size_t lengthToAllocate = x_allocationSize;
    if (unlikely(m_spdsPageAllocLimit > -x_allocationSize))
    {
        if (m_spdsPageAllocLimit == 0)
        {
            lengthToAllocate = 4096;
        }
        else
        {
            Assert(m_spdsPageAllocLimit < 0);
            lengthToAllocate = static_cast<size_t>(-m_spdsPageAllocLimit);
            Assert(lengthToAllocate <= x_allocationSize);
        }
    }
    Assert(lengthToAllocate > 0 && lengthToAllocate % x_pageSize == 0 && lengthToAllocate % x_spdsAllocationPageSize == 0);

    VM_FAIL_IF(SubWithOverflowCheck(m_spdsPageAllocLimit, static_cast<int32_t>(lengthToAllocate), &m_spdsPageAllocLimit),
               "Resource limit exceeded: SPDS region overflowed 2GB memory limit.");

    // We have some code that assumes the address std::numeric_limits<int32_t>::min() is not used
    //
    VM_FAIL_IF(m_spdsPageAllocLimit == std::numeric_limits<int32_t>::min(),
               "Resource limit exceeded: SPDS region overflowed 2GB memory limit.");

    // Allocate memory
    //
    uintptr_t allocAddr = VMBaseAddress() + SignExtendTo<uint64_t>(m_spdsPageAllocLimit);
    Assert(allocAddr % x_pageSize == 0 && allocAddr % x_spdsAllocationPageSize == 0);
    void* r = mmap(reinterpret_cast<void*>(allocAddr), static_cast<size_t>(lengthToAllocate), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_FIXED, -1, 0);
    VM_FAIL_WITH_ERRNO_IF(r == MAP_FAILED,
                          "Out of Memory: Allocation of length %llu failed", static_cast<unsigned long long>(lengthToAllocate));

    Assert(r == reinterpret_cast<void*>(allocAddr));

    // The first page is returned to caller
    //
    int32_t result = m_spdsPageAllocLimit + static_cast<int32_t>(x_spdsAllocationPageSize);

    // Insert the other pages, if any, into free list
    //
    size_t numPages = lengthToAllocate / x_spdsAllocationPageSize;
    if (numPages > 1)
    {
        int32_t cur = result + static_cast<int32_t>(x_spdsAllocationPageSize);
        int32_t firstPage = cur;
        for (size_t i = 1; i < numPages - 1; i++)
        {
            int32_t next = cur + static_cast<int32_t>(x_spdsAllocationPageSize);
            std::atomic<int32_t>* addr = reinterpret_cast<std::atomic<int32_t>*>(VMBaseAddress() + SignExtendTo<uint64_t>(cur) - 4);
            addr->store(next, std::memory_order_relaxed);
            cur = next;
        }

        int32_t lastPage = cur;
        Assert(lastPage == m_spdsPageAllocLimit + static_cast<int32_t>(lengthToAllocate));

        SpdsPutAllocatedPagesToFreeList(firstPage, lastPage);
    }
    return result;
}

bool WARN_UNUSED VM::InitializeVMGlobalData()
{
    m_filePointerForStdout = stdout;
    m_filePointerForStderr = stderr;

    m_somGlobals = nullptr;
    m_stringHiddenClass = nullptr;
    m_arrayHiddenClass = nullptr;
    m_objectClass = nullptr;
    m_classClass = nullptr;
    m_metaclassClass = nullptr;
    m_nilClass = nullptr;
    m_booleanClass = nullptr;
    m_trueClass = nullptr;
    m_falseClass = nullptr;
    m_integerClass = nullptr;
    m_doubleClass = nullptr;
    m_blockClass = nullptr;
    m_block1Class = nullptr;
    m_block2Class = nullptr;
    m_block3Class = nullptr;
    m_methodClass = nullptr;
    m_symbolClass = nullptr;
    m_primitiveClass = nullptr;
    m_systemClass = nullptr;
    m_metaclassClassLoaded = false;

    m_cachedSingleCharStrings = new TValue[256];
    memset(m_cachedSingleCharStrings, 0, sizeof(TValue) * 256);

    m_unknownGlobalHandler = GetUniquedString("unknownGlobal:");
    m_doesNotUnderstandHandler = GetUniquedString("doesNotUnderstand:arguments:");
    m_escapedBlockHandler = GetUniquedString("escapedBlock:");

    // If you add new stuffs here, make sure to change IsSelectorArithmeticOperator correspondingly.
    //
    m_strOperatorPlus = GetUniquedString("+");
    m_strOperatorMinus = GetUniquedString("-"); ReleaseAssert(m_strOperatorMinus.m_id == m_strOperatorPlus.m_id + 1);
    m_strOperatorStar = GetUniquedString("*"); ReleaseAssert(m_strOperatorStar.m_id == m_strOperatorMinus.m_id + 1);
    m_strOperatorSlashSlash = GetUniquedString("//"); ReleaseAssert(m_strOperatorSlashSlash.m_id == m_strOperatorStar.m_id + 1);
    m_strOperatorPercent = GetUniquedString("%"); ReleaseAssert(m_strOperatorPercent.m_id == m_strOperatorSlashSlash.m_id + 1);
    m_strOperatorAnd = GetUniquedString("&"); ReleaseAssert(m_strOperatorAnd.m_id == m_strOperatorPercent.m_id + 1);
    m_strOperatorEqual = GetUniquedString("="); ReleaseAssert(m_strOperatorEqual.m_id == m_strOperatorAnd.m_id + 1);
    m_strOperatorLessThan = GetUniquedString("<"); ReleaseAssert(m_strOperatorLessThan.m_id == m_strOperatorEqual.m_id + 1);
    m_strOperatorLessEqual = GetUniquedString("<="); ReleaseAssert(m_strOperatorLessEqual.m_id == m_strOperatorLessThan.m_id + 1);
    m_strOperatorGreaterThan = GetUniquedString(">"); ReleaseAssert(m_strOperatorGreaterThan.m_id == m_strOperatorLessEqual.m_id + 1);
    m_strOperatorGreaterEqual = GetUniquedString(">="); ReleaseAssert(m_strOperatorGreaterEqual.m_id == m_strOperatorGreaterThan.m_id + 1);
    m_strOperatorUnequal = GetUniquedString("<>"); ReleaseAssert(m_strOperatorUnequal.m_id == m_strOperatorGreaterEqual.m_id + 1);
    m_strOperatorTildeUnequal = GetUniquedString("~="); ReleaseAssert(m_strOperatorTildeUnequal.m_id == m_strOperatorUnequal.m_id + 1);
    m_strOperatorLeftShift = GetUniquedString("<<"); ReleaseAssert(m_strOperatorLeftShift.m_id == m_strOperatorTildeUnequal.m_id + 1);
    m_strOperatorRightShift = GetUniquedString(">>>"); ReleaseAssert(m_strOperatorRightShift.m_id == m_strOperatorLeftShift.m_id + 1);
    m_strOperatorBitwiseXor = GetUniquedString("bitXor:"); ReleaseAssert(m_strOperatorBitwiseXor.m_id == m_strOperatorRightShift.m_id + 1);
    m_strOperatorSlash = GetUniquedString("/"); ReleaseAssert(m_strOperatorSlash.m_id == m_strOperatorBitwiseXor.m_id + 1);
    m_strOperatorEqualEqual = GetUniquedString("=="); ReleaseAssert(m_strOperatorEqualEqual.m_id == m_strOperatorSlash.m_id + 1);
    ReleaseAssert(m_strOperatorEqualEqual.m_id == m_strOperatorPlus.m_id + 17);

    // If you add new stuffs here, make sure to change IsSelectorInlinableControlFlow correspondingly.
    //
    m_stringIdForWhileTrue = m_interner.InternString("whileTrue:");
    m_stringIdForWhileFalse = m_interner.InternString("whileFalse:"); ReleaseAssert(m_stringIdForWhileFalse == m_stringIdForWhileTrue + 1);
    m_stringIdForIfTrueIfFalse = m_interner.InternString("ifTrue:ifFalse:"); ReleaseAssert(m_stringIdForIfTrueIfFalse == m_stringIdForWhileFalse + 1);
    m_stringIdForIfFalseIfTrue = m_interner.InternString("ifFalse:ifTrue:"); ReleaseAssert(m_stringIdForIfFalseIfTrue == m_stringIdForIfTrueIfFalse + 1);
    m_stringIdForIfNilIfNotNil = m_interner.InternString("ifNil:ifNotNil:"); ReleaseAssert(m_stringIdForIfNilIfNotNil == m_stringIdForIfFalseIfTrue + 1);
    m_stringIdForIfNotNilIfNil = m_interner.InternString("ifNotNil:ifNil:"); ReleaseAssert(m_stringIdForIfNotNilIfNil == m_stringIdForIfNilIfNotNil + 1);
    m_stringIdForIfTrue = m_interner.InternString("ifTrue:"); ReleaseAssert(m_stringIdForIfTrue == m_stringIdForIfNotNilIfNil + 1);
    m_stringIdForIfFalse = m_interner.InternString("ifFalse:"); ReleaseAssert(m_stringIdForIfFalse == m_stringIdForIfTrue + 1);
    m_stringIdForIfNil = m_interner.InternString("ifNil:"); ReleaseAssert(m_stringIdForIfNil == m_stringIdForIfFalse + 1);
    m_stringIdForIfNotNil = m_interner.InternString("ifNotNil:"); ReleaseAssert(m_stringIdForIfNotNil == m_stringIdForIfNil + 1);
    m_stringIdForOperatorAnd = m_interner.InternString("&&"); ReleaseAssert(m_stringIdForOperatorAnd == m_stringIdForIfNotNil + 1);
    m_stringIdForOperatorOr = m_interner.InternString("||"); ReleaseAssert(m_stringIdForOperatorOr == m_stringIdForOperatorAnd + 1);
    m_stringIdForMethodAnd = m_interner.InternString("and:"); ReleaseAssert(m_stringIdForMethodAnd == m_stringIdForOperatorOr + 1);
    m_stringIdForMethodOr = m_interner.InternString("or:"); ReleaseAssert(m_stringIdForMethodOr == m_stringIdForMethodAnd + 1);
    m_stringIdForToDo = m_interner.InternString("to:do:"); ReleaseAssert(m_stringIdForToDo == m_stringIdForMethodOr + 1);
    m_stringIdForDowntoDo = m_interner.InternString("downTo:do:"); ReleaseAssert(m_stringIdForDowntoDo == m_stringIdForToDo + 1);
    ReleaseAssert(m_stringIdForDowntoDo == m_stringIdForWhileTrue + 15);

    // Note that "&&", "||", "and:", and "or:" are also inlinable control flow structure
    //
    m_strOperatorLogicalAnd = GetUniquedString("&&");
    m_strOperatorLogicalOr = GetUniquedString("||");
    m_strOperatorKeywordAnd = GetUniquedString("and:");
    m_strOperatorKeywordOr = GetUniquedString("or:");
    m_strOperatorValueColon = GetUniquedString("value:");
    m_strOperatorAtColon = GetUniquedString("at:");
    m_strOperatorCharAtColon = GetUniquedString("charAt:");

    // If you add new stuffs here, make sure to change IsSelectorSpecializableUnaryOperator correspondingly.
    //
    m_strOperatorAbs = GetUniquedString("abs");
    m_strOperatorSqrt = GetUniquedString("sqrt"); ReleaseAssert(m_strOperatorSqrt.m_id == m_strOperatorAbs.m_id + 1);
    m_strOperatorIsNil = GetUniquedString("isNil"); ReleaseAssert(m_strOperatorIsNil.m_id == m_strOperatorSqrt.m_id + 1);
    m_strOperatorNotNil = GetUniquedString("notNil"); ReleaseAssert(m_strOperatorNotNil.m_id == m_strOperatorIsNil.m_id + 1);
    m_strOperatorValue = GetUniquedString("value"); ReleaseAssert(m_strOperatorValue.m_id == m_strOperatorNotNil.m_id + 1);
    m_strOperatorNot = GetUniquedString("not"); ReleaseAssert(m_strOperatorNot.m_id == m_strOperatorValue.m_id + 1);
    m_strOperatorLength = GetUniquedString("length"); ReleaseAssert(m_strOperatorLength.m_id == m_strOperatorNot.m_id + 1);

    m_strOperatorAtPut = GetUniquedString("at:put:");
    m_strOperatorValueWith = GetUniquedString("value:with:");

    CreateRootCoroutine();
    return true;
}

bool WARN_UNUSED VM::Initialize()
{
    static_assert(x_segmentRegisterSelfReferencingOffset == offsetof_member_v<&VM::m_self>);

    CHECK_LOG_ERROR(InitializeVMBase());
    CHECK_LOG_ERROR(InitializeVMGlobalData());
    return true;
}

void VM::Cleanup()
{
}

void VM::CreateRootCoroutine()
{
    // Create global object
    //
    UserHeapPointer<void> globalObject;
    m_rootCoroutine = CoroutineRuntimeContext::Create(this, globalObject, 65536 /*numStackSlots*/);
    m_rootCoroutine->m_coroutineStatus.SetResumable(false);
    m_rootCoroutine->m_parent = nullptr;
}

SOMObject* VM::GetInternedString(size_t ord)
{
    TestAssert(ord < m_interner.m_list.size());
    if (ord >= m_internedStringObjects.size())
    {
        m_internedStringObjects.resize(ord + 1, nullptr);
    }
    if (m_internedStringObjects[ord] != nullptr)
    {
        return m_internedStringObjects[ord];
    }
    SOMObject* str = SOMObject::AllocateString(m_interner.m_list[ord].first);
    m_internedStringObjects[ord] = str;
    return str;
}

SOMObject* VM::GetInternedSymbol(size_t ord)
{
    TestAssert(ord < m_interner.m_list.size());
    if (ord >= m_internedSymbolObjects.size())
    {
        m_internedSymbolObjects.resize(ord + 1, nullptr);
    }
    if (m_internedSymbolObjects[ord] != nullptr)
    {
        return m_internedSymbolObjects[ord];
    }
    TestAssert(m_symbolClass != nullptr);
    SOMObject* str = SOMObject::AllocateString(m_interner.m_list[ord].first);
    str->m_hiddenClass = SystemHeapPointer<SOMClass>(m_symbolClass).m_value;
    m_internedSymbolObjects[ord] = str;
    return str;
}
