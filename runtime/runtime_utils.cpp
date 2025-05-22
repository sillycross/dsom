#include "runtime_utils.h"
#include "deegen_options.h"
#include "vm.h"
#include "deegen_enter_vm_from_c.h"

#include "generated/get_guest_language_function_interpreter_entry_point.h"
#include "json_utils.h"
#include "bytecode_builder.h"
#include "drt/baseline_jit_codegen_helper.h"

const size_t x_num_bytecode_metadata_struct_kinds_ = x_num_bytecode_metadata_struct_kinds;

void* WARN_UNUSED UnlinkedCodeBlock::GetInterpreterEntryPoint()
{
    return generated::GetGuestLanguageFunctionEntryPointForInterpreter(m_hasVariadicArguments, m_numFixedArguments);
}

CodeBlock* WARN_UNUSED CodeBlock::Create(VM* vm, UnlinkedCodeBlock* ucb, UserHeapPointer<void> globalObject)
{
    Assert(ucb->m_bytecodeMetadataLength % 8 == 0);
    size_t sizeToAllocate = GetTrailingArrayOffset() + ucb->m_bytecodeMetadataLength + sizeof(TValue) * ucb->m_cstTableLength + RoundUpToMultipleOf<8>(ucb->m_bytecodeLengthIncludingTailPadding);
    uint8_t* addressBegin = TranslateToRawPointer(vm, vm->AllocFromSystemHeap(static_cast<uint32_t>(sizeToAllocate)).AsNoAssert<uint8_t>());
    memcpy(addressBegin, ucb->m_cstTable, sizeof(TValue) * ucb->m_cstTableLength);

    CodeBlock* cb = reinterpret_cast<CodeBlock*>(addressBegin + sizeof(TValue) * ucb->m_cstTableLength);
    ConstructInPlace(cb);
    cb->SystemHeapGcObjectHeader::Populate<ExecutableCode*>(cb);
    cb->m_executableCodeKind = Kind::BytecodeFunction;
    cb->m_hasVariadicArguments = ucb->m_hasVariadicArguments;
    cb->m_numFixedArguments = ucb->m_numFixedArguments;
    cb->m_globalObject = globalObject;
    cb->m_stackFrameNumSlots = ucb->m_stackFrameNumSlots;
    cb->m_numUpvalues = ucb->m_numUpvalues;
    cb->m_owner = ucb;
    cb->m_bestEntryPoint = ucb->GetInterpreterEntryPoint();
    cb->m_bytecodeLengthIncludingTailPadding = ucb->m_bytecodeLengthIncludingTailPadding;
    cb->m_bytecodeMetadataLength = ucb->m_bytecodeMetadataLength;
    cb->m_baselineCodeBlock = nullptr;
    cb->m_dfgCodeBlock = nullptr;
    cb->m_fnTyMask  = static_cast<uint8_t>(static_cast<uint8_t>(ucb->m_fnKind) + 16 * static_cast<uint8_t>(ucb->m_trivialFnType));
    if (ucb->m_trivialFnType == SOM_LiteralReturn || ucb->m_trivialFnType == SOM_GlobalReturn || ucb->m_trivialFnType == SOM_Getter || ucb->m_trivialFnType == SOM_Setter)
    {
        TestAssert(ucb->m_numUpvalues == 0);
        cb->m_needExtraUpvalueDueToTrivialFn = true;
        cb->m_trivialFnExtraInfo = ucb->m_trivialFnInfo;
    }
    else
    {
        cb->m_needExtraUpvalueDueToTrivialFn = false;
        cb->m_trivialFnExtraInfo = 0;
    }
    if (vm->InterpreterCanTierUpFurther())
    {
        cb->m_interpreterTierUpCounter = x_interpreter_tier_up_threshold_bytecode_length_multiplier * ucb->m_bytecodeLengthIncludingTailPadding;
    }
    else
    {
        // We increment counter on forward edges, choose 2^62 to avoid overflow.
        //
        cb->m_interpreterTierUpCounter = 1LL << 62;
    }
    memcpy(cb->GetBytecodeStream(), ucb->m_bytecode, ucb->m_bytecodeLengthIncludingTailPadding);

    ForEachBytecodeMetadata(cb, []<typename T>(T* md) ALWAYS_INLINE {
        md->Init();
    });

    // Immediately compile the CodeBlock to baseline JIT code if requested by user.
    // Note that this must be done after we have set up all the fields in the CodeBlock
    //
    if (vm->IsEngineStartingTierBaselineJit())
    {
        BaselineCodeBlock* bcb = deegen_baseline_jit_do_codegen(cb);
        Assert(cb->m_baselineCodeBlock == bcb);
        Assert(cb->m_bestEntryPoint != ucb->GetInterpreterEntryPoint());
        Assert(cb->m_bestEntryPoint == bcb->m_jitCodeEntry);
        std::ignore = bcb;
    }

    return cb;
}

void CodeBlock::UpdateBestEntryPoint(void* newEntryPoint)
{
    void* oldBestEntryPoint = m_bestEntryPoint;

    // Update all interpreter call IC to use the new entry point
    //
    {
        uint8_t* endAnchor = reinterpret_cast<uint8_t*>(&m_interpreterCallIcList);
        uint8_t* curAnchor = endAnchor;
        while (true)
        {
            curAnchor -= UnalignedLoad<int32_t>(curAnchor + 4);
            if (curAnchor == endAnchor)
            {
                break;
            }
            // We rely on the ABI layout that the codePtr resides right before the doubly link
            //
            Assert(UnalignedLoad<void*>(curAnchor - 8) == oldBestEntryPoint);
            UnalignedStore<void*>(curAnchor - 8, newEntryPoint);
        }
    }

    // Update all JIT call IC to use the new entry point
    //
    {
        uint64_t diff = reinterpret_cast<uint64_t>(newEntryPoint) - reinterpret_cast<uint64_t>(oldBestEntryPoint);
        for (JitCallInlineCacheEntry* icEntry : m_jitCallIcList.elements())
        {
            icEntry->UpdateTargetFunctionCodePtr(diff);
        }
    }

    // Update m_bestEntryPoint so uncached calls also use the new entry point
    //
    m_bestEntryPoint = newEntryPoint;
}

std::pair<TValue* /*retStart*/, uint64_t /*numRet*/> VM::LaunchScript(ScriptModule* module)
{
    CoroutineRuntimeContext* rc = GetRootCoroutine();
    return DeegenEnterVMFromC(rc, module->m_defaultEntryPoint.As(), rc->m_stackBegin);
}

UserHeapPointer<FunctionObject> WARN_UNUSED NO_INLINE FunctionObject::CreateAndFillUpvalues(
    CodeBlock* cb, CoroutineRuntimeContext* rc, TValue* stackFrameBase, HeapPtr<FunctionObject> parent, size_t selfOrdinalInStackFrame)
{
    VM* vm = VM::GetActiveVMForCurrentThread();
    UnlinkedCodeBlock* ucb = cb->m_owner;
    HeapPtr<FunctionObject> r = Create(vm, cb).As();
    uint32_t numUpvalues = cb->m_numUpvalues;
    AssertImp(numUpvalues > 0, TranslateToRawPointer(TCGet(parent->m_executable).As())->IsBytecodeFunction());
    AssertImp(numUpvalues > 0, cb->m_owner->m_parent == static_cast<HeapPtr<CodeBlock>>(TCGet(parent->m_executable).As())->m_owner);
    UpvalueMetadata* upvalueInfo = ucb->m_upvalueInfo;
    for (uint32_t ord = 0; ord < numUpvalues; ord++)
    {
        UpvalueMetadata& uvmt = upvalueInfo[ord];
        Assert(uvmt.m_immutabilityFieldFinalized);
        if (uvmt.m_isParentLocal)
        {
            if (uvmt.m_isImmutable)
            {
                TValue uv;
                if (uvmt.m_slot == selfOrdinalInStackFrame)
                {
                    uv = TValue::Create<tFunction>(r);
                }
                else
                {
                    uv = stackFrameBase[uvmt.m_slot];
                }
                TCSet(r->m_upvalues[ord], uv);
            }
            else
            {
                Upvalue* uvPtr = TranslateToRawPointer(vm, Upvalue::Create(rc, stackFrameBase + uvmt.m_slot, uvmt.m_isImmutable));
                TestAssert(uvPtr->m_type == HeapEntityType::Upvalue);
                TValue tv;
                tv.m_value = reinterpret_cast<uint64_t>(uvPtr);
                TCSet(r->m_upvalues[ord], tv);
            }
        }
        else
        {
            TValue uv = FunctionObject::GetMutableUpvaluePtrOrImmutableUpvalue(parent, uvmt.m_slot);
            AssertImp(!uvmt.m_isImmutable, reinterpret_cast<Upvalue*>(uv.m_value)->m_type == HeapEntityType::Upvalue);
            TCSet(r->m_upvalues[ord], uv);
        }
    }
    if (cb->m_needExtraUpvalueDueToTrivialFn)
    {
        TestAssert(cb->m_numUpvalues == 0 && r->m_numUpvalues == 1);
        r->m_upvalues[0].m_value = cb->m_trivialFnExtraInfo;
    }
    return r;
}

FunctionObject* WARN_UNUSED NO_INLINE FunctionObject::CreateForDfgAndFillUpvaluesFromParent(
    UnlinkedCodeBlock* ucb, HeapPtr<FunctionObject> parent)
{
    VM* vm = VM::GetActiveVMForCurrentThread();
    ExecutableCode* parentEc = TranslateToRawPointer(vm, TCGet(parent->m_executable).As());
    TestAssert(parentEc->IsBytecodeFunction());
    CodeBlock* parentCb = static_cast<CodeBlock*>(parentEc);
    CodeBlock* cb = ucb->GetCodeBlock(parentCb->m_globalObject);
    TestAssert(cb->m_owner == ucb);
    TestAssert(ucb->m_parent == parentCb->m_owner);

    FunctionObject* r = TranslateToRawPointer(vm, Create(vm, cb).As());

    uint32_t numUpvalues = cb->m_numUpvalues;
    UpvalueMetadata* upvalueInfo = ucb->m_upvalueInfo;
    for (uint32_t ord = 0; ord < numUpvalues; ord++)
    {
        UpvalueMetadata& uvmt = upvalueInfo[ord];
        Assert(uvmt.m_immutabilityFieldFinalized);
        if (!uvmt.m_isParentLocal)
        {
            TValue uv = FunctionObject::GetMutableUpvaluePtrOrImmutableUpvalue(parent, uvmt.m_slot);
            TestAssertImp(!uvmt.m_isImmutable, reinterpret_cast<Upvalue*>(uv.m_value)->m_type == HeapEntityType::Upvalue);
            r->m_upvalues[ord] = uv;
        }
    }
    if (cb->m_needExtraUpvalueDueToTrivialFn)
    {
        TestAssert(cb->m_numUpvalues == 0 && r->m_numUpvalues == 1);
        r->m_upvalues[0].m_value = cb->m_trivialFnExtraInfo;
    }
    return r;
}

CoroutineRuntimeContext* CoroutineRuntimeContext::Create(VM* vm, UserHeapPointer<void> globalObject, size_t numStackSlots)
{
    CoroutineRuntimeContext* r = TranslateToRawPointer(vm, vm->AllocFromUserHeap(static_cast<uint32_t>(sizeof(CoroutineRuntimeContext))).AsNoAssert<CoroutineRuntimeContext>());
    UserHeapGcObjectHeader::Populate(r);
    r->m_hiddenClass = x_hiddenClassForCoroutineRuntimeContext;
    r->m_coroutineStatus = CoroutineStatus::CreateInitStatus();
    r->m_globalObject = globalObject;
    r->m_numVariadicRets = 0;
    r->m_variadicRetStart = nullptr;
    r->m_upvalueList.m_value = 0;
    size_t bytesToAllocate = numStackSlots * sizeof(TValue);
    bytesToAllocate = RoundUpToMultipleOf<VM::x_pageSize>(bytesToAllocate);
    void* stackAreaWithOverflowProtection = mmap(nullptr, bytesToAllocate + x_stackOverflowProtectionAreaSize * 2,
                                                 PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    VM_FAIL_WITH_ERRNO_IF(stackAreaWithOverflowProtection == MAP_FAILED,
                          "Failed to reserve address range of length %llu",
                          static_cast<unsigned long long>(bytesToAllocate + x_stackOverflowProtectionAreaSize * 2));

    void* stackArea = mmap(reinterpret_cast<uint8_t*>(stackAreaWithOverflowProtection) + x_stackOverflowProtectionAreaSize,
                           bytesToAllocate, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    VM_FAIL_WITH_ERRNO_IF(stackArea == MAP_FAILED,
                          "Out of Memory: Allocation of length %llu failed", static_cast<unsigned long long>(bytesToAllocate));
    Assert(stackArea == reinterpret_cast<uint8_t*>(stackAreaWithOverflowProtection) + x_stackOverflowProtectionAreaSize);
    r->m_stackBegin = reinterpret_cast<TValue*>(stackArea);
    return r;
}

BaselineCodeBlock* WARN_UNUSED BaselineCodeBlock::Create(CodeBlock* cb,
                                                         uint32_t numBytecodes,
                                                         uint32_t slowPathDataStreamLength,
                                                         void* jitCodeEntry,
                                                         void* jitRegionStart,
                                                         uint32_t jitRegionSize)
{
    size_t numEntriesInConstantTable = cb->m_owner->m_cstTableLength;
    static_assert(alignof(BaselineCodeBlock) == 8);         // the computation below relies on this
    size_t sizeToAllocate = sizeof(TValue) * numEntriesInConstantTable + GetTrailingArrayOffset() + sizeof(SlowPathDataAndBytecodeOffset) * numBytecodes + slowPathDataStreamLength;
    sizeToAllocate = RoundUpToMultipleOf<8>(sizeToAllocate);

    VM* vm = VM::GetActiveVMForCurrentThread();
    uint8_t* addressBegin = TranslateToRawPointer(vm, vm->AllocFromSystemHeap(static_cast<uint32_t>(sizeToAllocate)).AsNoAssert<uint8_t>());
    memcpy(addressBegin, cb->m_owner->m_cstTable, sizeof(TValue) * numEntriesInConstantTable);

    BaselineCodeBlock* res = reinterpret_cast<BaselineCodeBlock*>(addressBegin + sizeof(TValue) * numEntriesInConstantTable);
    ConstructInPlace(res);
    res->m_jitCodeEntry = jitCodeEntry;
    res->m_owner = cb;
    res->m_globalObject = cb->m_globalObject;
    res->m_numBytecodes = numBytecodes;
    res->m_stackFrameNumSlots = cb->m_stackFrameNumSlots;
    res->m_maxObservedNumVariadicArgs = 0;
    res->m_slowPathDataStreamLength = slowPathDataStreamLength;
    res->m_jitRegionStart = jitRegionStart;
    res->m_jitRegionSize = jitRegionSize;

    TestAssert(cb->m_baselineCodeBlock == nullptr);
    cb->m_baselineCodeBlock = res;

    return res;
}

JitCallInlineCacheEntry* WARN_UNUSED JitCallInlineCacheEntry::Create(VM* vm,
                                                                     ExecutableCode* targetExecutableCode,
                                                                     SpdsPtr<JitCallInlineCacheEntry> callSiteNextNode,
                                                                     HeapPtr<void> entity,
                                                                     size_t icTraitKind)
{
    JitCallInlineCacheEntry* entry = vm->AllocateFromSpdsRegionUninitialized<JitCallInlineCacheEntry>();
    ConstructInPlace(entry);
    entry->m_callSiteNextNode = callSiteNextNode;
    entry->m_entity = entity;
    Assert(icTraitKind <= std::numeric_limits<uint16_t>::max());
    const JitCallInlineCacheTraits* trait = deegen_jit_call_inline_cache_trait_table[icTraitKind];

    AssertImp(trait->m_isDirectCallMode, entry->m_entity.IsUserHeapPointer() && entry->m_entity.As<UserHeapGcObjectHeader>()->m_type == HeapEntityType::Function);
    AssertImp(!trait->m_isDirectCallMode, !entry->m_entity.IsUserHeapPointer() && entry->m_entity.As<SystemHeapGcObjectHeader>()->m_type == HeapEntityType::ExecutableCode);
    AssertImp(!trait->m_isDirectCallMode, targetExecutableCode == TranslateToRawPointer(vm, entity));

    void* regionVoidPtr = vm->GetJITMemoryAlloc()->AllocateGivenStepping(trait->m_jitCodeAllocationLengthStepping);

    Assert(reinterpret_cast<uint64_t>(regionVoidPtr) < (1ULL << 48));
    entry->m_taggedPtr = reinterpret_cast<uint64_t>(regionVoidPtr) | (static_cast<uint64_t>(icTraitKind) << 48);

    Assert(entry->GetJitRegionStart() == regionVoidPtr);
    Assert(entry->GetIcTrait() == trait);

    Assert(!entry->IsOnDoublyLinkedList());
    if (targetExecutableCode->IsBytecodeFunction())
    {
        CodeBlock* targetCb = static_cast<CodeBlock*>(targetExecutableCode);
        targetCb->m_jitCallIcList.InsertAtHead(entry);
        Assert(entry->IsOnDoublyLinkedList());
    }

    return entry;
}

void JitCallInlineCacheEntry::Destroy(VM* vm)
{
    AssertIff(IsOnDoublyLinkedList(), GetTargetExecutableCode(vm)->IsBytecodeFunction());
    if (IsOnDoublyLinkedList())
    {
        RemoveFromDoublyLinkedList();
    }
    vm->GetJITMemoryAlloc()->Free(GetJitRegionStart());
    vm->DeallocateSpdsRegionObject(this);
}

void* WARN_UNUSED JitCallInlineCacheSite::InsertInDirectCallMode(uint16_t dcIcTraitKind, TValue tv, uint8_t* transitedToCCMode /*out*/)
{
    Assert(m_numEntries < x_maxEntries);
    Assert(m_mode == Mode::DirectCall);
    Assert(tv.Is<tFunction>());

    VM* vm = VM::GetActiveVMForCurrentThread();

    // Compute bloom filter hash mask
    //
    uint16_t bloomFilterMask;
    {
        uint64_t hashValue64 = HashPrimitiveTypes(tv.As<tFunction>()->m_executable.m_value);
        bloomFilterMask = static_cast<uint16_t>((1 << (hashValue64 & 15)) | (1 << ((hashValue64 >> 8) & 15)));
    }

    ExecutableCode* targetEc = TranslateToRawPointer(vm, TCGet(tv.As<tFunction>()->m_executable).As());

    // Figure out if we shall transition to closure call mode, we do this when we notice that
    // the passed-in call target has the same ExecutableCode as an existing cached target
    //
    bool shouldTransitToCCMode = false;
    if (unlikely((m_bloomFilter & bloomFilterMask) == bloomFilterMask))
    {
        SpdsPtr<JitCallInlineCacheEntry> linkListNode = TCGet(m_linkedListHead);
        // if the IC site is empty, m_bloomFilter should be 0 and the above check shall never pass
        //
        Assert(!linkListNode.IsInvalidPtr());
        do {
            JitCallInlineCacheEntry* entry = TranslateToRawPointer(vm, linkListNode.AsPtr());
            Assert(entry->GetIcTraitKind() == dcIcTraitKind);
            if (entry->GetTargetExecutableCodeKnowingDirectCall(vm) == targetEc)
            {
                shouldTransitToCCMode = true;
                break;
            }
            linkListNode = TCGet(entry->m_callSiteNextNode);
        } while (!linkListNode.IsInvalidPtr());
    }

#ifndef NDEBUG
    // In debug mode, validate that the shouldTransitToCCMode decision is correct by brute force,
    // and also validate that assorted information of the linked list is as expected
    //
    {
        std::unordered_set<ExecutableCode*> checkUnique;
        bool goldDecision = false;
        SpdsPtr<JitCallInlineCacheEntry> linkListNode = TCGet(m_linkedListHead);
        while (!linkListNode.IsInvalidPtr())
        {
            JitCallInlineCacheEntry* entry = TranslateToRawPointer(vm, linkListNode.AsPtr());
            Assert(entry->GetIcTraitKind() == dcIcTraitKind);

            // We should never reach here if the IC ought to hit
            //
            Assert(entry->m_entity.IsUserHeapPointer());
            Assert(entry->m_entity.As() != tv.As<tFunction>());

            // All the ExecutableCode in the IC list should be distinct
            //
            ExecutableCode* ec = entry->GetTargetExecutableCodeKnowingDirectCall(vm);
            Assert(!checkUnique.count(ec));
            checkUnique.insert(ec);

            if (ec == targetEc)
            {
                goldDecision = true;
            }

            linkListNode = TCGet(entry->m_callSiteNextNode);
        }
        Assert(checkUnique.size() == m_numEntries);
        Assert(goldDecision == shouldTransitToCCMode);
    }
#endif

    if (likely(!shouldTransitToCCMode))
    {
        // No transition to closure-call mode, just create a new IC entry for the target
        //
        m_bloomFilter |= bloomFilterMask;
        m_numEntries++;

        JitCallInlineCacheEntry* newEntry = JitCallInlineCacheEntry::Create(vm,
                                                                            targetEc,
                                                                            TCGet(m_linkedListHead) /*callSiteNextNode*/,
                                                                            tv.As<tFunction>(),
                                                                            dcIcTraitKind);
        TCSet(m_linkedListHead, SpdsPtr<JitCallInlineCacheEntry> { newEntry });

        *transitedToCCMode = 0;
        return newEntry->GetJitRegionStart();
    }

    // We need to transit to closure-call mode
    //
    {
        // Invalidate all existing ICs
        //
        SpdsPtr<JitCallInlineCacheEntry> node = TCGet(m_linkedListHead);
        Assert(!node.IsInvalidPtr());
        do {
            JitCallInlineCacheEntry* entry = TranslateToRawPointer(vm, node.AsPtr());
            node = TCGet(entry->m_callSiteNextNode);
            entry->Destroy(vm);
        } while (!node.IsInvalidPtr());
    }

    // Create the new IC entry
    //
    JitCallInlineCacheEntry* entry = JitCallInlineCacheEntry::Create(vm,
                                                                     targetEc,
                                                                     SpdsPtr<JitCallInlineCacheEntry> { 0 } /*callSiteNextNode*/,
                                                                     TranslateToHeapPtr(targetEc),
                                                                     dcIcTraitKind + 1 /*icTraitKind*/);
    TCSet(m_linkedListHead, SpdsPtr<JitCallInlineCacheEntry> { entry });
    m_mode = (m_numEntries > 1) ? Mode::ClosureCallWithMoreThanOneTargetObserved : Mode::ClosureCall;
    m_numEntries = 1;
    m_bloomFilter = 0;

    *transitedToCCMode = 1;
    return entry->GetJitRegionStart();
}

void* WARN_UNUSED JitCallInlineCacheSite::InsertInClosureCallMode(uint16_t dcIcTraitKind, TValue tv)
{
    Assert(m_numEntries < x_maxEntries);
    Assert(m_mode == Mode::ClosureCall || m_mode == Mode::ClosureCallWithMoreThanOneTargetObserved);
    Assert(tv.Is<tFunction>());

    VM* vm = VM::GetActiveVMForCurrentThread();
    ExecutableCode* targetEc = TranslateToRawPointer(vm, TCGet(tv.As<tFunction>()->m_executable).As());

#ifndef NDEBUG
    // In debug mode, validate that assorted information of the linked list is as expected
    //
    {
        std::unordered_set<ExecutableCode*> checkUnique;
        SpdsPtr<JitCallInlineCacheEntry> linkListNode = TCGet(m_linkedListHead);
        while (!linkListNode.IsInvalidPtr())
        {
            JitCallInlineCacheEntry* entry = TranslateToRawPointer(vm, linkListNode.AsPtr());
            Assert(entry->GetIcTraitKind() == dcIcTraitKind + 1);
            Assert(!entry->GetIcTrait()->m_isDirectCallMode);

            // We should never reach here if the IC ought to hit
            //
            ExecutableCode* ec = entry->GetTargetExecutableCode(vm);
            Assert(ec != targetEc);

            // All the ExecutableCode in the IC list should be distinct
            //
            Assert(!checkUnique.count(ec));
            checkUnique.insert(ec);

            linkListNode = TCGet(entry->m_callSiteNextNode);
        }
        Assert(checkUnique.size() == m_numEntries);
    }
#endif

    // Create the new IC entry
    //
    JitCallInlineCacheEntry* entry = JitCallInlineCacheEntry::Create(vm,
                                                                     targetEc,
                                                                     TCGet(m_linkedListHead) /*callSiteNextNode*/,
                                                                     TranslateToHeapPtr(targetEc),
                                                                     dcIcTraitKind + 1 /*icTraitKind*/);
    TCSet(m_linkedListHead, SpdsPtr<JitCallInlineCacheEntry> { entry });
    m_numEntries++;
    return entry->GetJitRegionStart();
}
