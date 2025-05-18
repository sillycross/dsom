#pragma once

#include "common_utils.h"
#include "memory_ptr.h"
#include "vm.h"
#include "spds_doubly_linked_list.h"
#include "baseline_jit_codegen_helper.h"
#include "bytecode_builder_utils.h"
#include "som_class.h"

class StackFrameHeader;
class CodeBlock;

class Upvalue;

struct CoroutineStatus
{
    // Must start with the coroutine distinguish-bit set because this class occupies the ArrayType field
    //
    constexpr CoroutineStatus() : m_asValue(0) { }
    explicit constexpr CoroutineStatus(uint8_t value) : m_asValue(value) { }

    // True iff it is legal to use 'coroutine.resume' on this coroutine.
    //
    using BFM_isResumable = BitFieldMember<uint8_t, bool /*type*/, 0 /*start*/, 1 /*width*/>;
    constexpr bool IsResumable() { return BFM_isResumable::Get(m_asValue); }
    constexpr void SetResumable(bool v) { return BFM_isResumable::Set(m_asValue, v); }

    // True iff this coroutine is dead (has either finished execution or thrown an error)
    // Note that if IsDead() == true, IsResumable() must be false
    //
    using BFM_isDead = BitFieldMember<uint8_t, bool /*type*/, 1 /*start*/, 1 /*width*/>;
    constexpr bool IsDead() { return BFM_isDead::Get(m_asValue); }
    constexpr void SetDead(bool v) { return BFM_isDead::Set(m_asValue, v); }

    // To summarize, the possible states are:
    // (1) IsResumable() => suspended coroutine
    // (2) IsDead() => dead coroutine (finished execution or errored out)
    // (3) !IsResumable() && !IsDead() => a coroutine in the stack of the active coroutines
    //

    static constexpr CoroutineStatus CreateInitStatus()
    {
        CoroutineStatus res;
        res.SetResumable(true);
        res.SetDead(false);
        return res;
    }

    // Load the ArrayType field of any object, and use this function to validate that the object is
    // indeed a coroutine object and the coroutine object is resumable in one branch
    //
    static constexpr bool WARN_UNUSED IsCoroutineObjectAndResumable(uint8_t arrTypeField)
    {
        constexpr uint8_t maskToCheck = BFM_isResumable::x_maskForGet;
        bool result = (arrTypeField & maskToCheck) == maskToCheck;
        return result;
    }

    uint8_t m_asValue;
};
// Must be one byte because it occupies the ArrayType field
//
static_assert(sizeof(CoroutineStatus) == 1);

class alignas(8) CoroutineRuntimeContext
{
public:
    static constexpr uint32_t x_hiddenClassForCoroutineRuntimeContext = 0x10;
    static constexpr size_t x_defaultStackSlots = 4096;
    static constexpr size_t x_rootCoroutineDefaultStackSlots = 16384;
    static constexpr size_t x_stackOverflowProtectionAreaSize = 65536;
    static_assert(x_stackOverflowProtectionAreaSize % VM::x_pageSize == 0);

    static CoroutineRuntimeContext* Create(VM* vm, UserHeapPointer<void> globalObject, size_t numStackSlots = x_defaultStackSlots);

    void CloseUpvalues(TValue* base);

    uint32_t m_hiddenClass;  // Always x_hiddenClassForCoroutineRuntimeContext
    HeapEntityType m_type;
    GcCellState m_cellState;
    uint8_t m_reserved1;
    CoroutineStatus m_coroutineStatus;

    // m_variadicRetStart[ord] holds variadic return value 'ord'
    // TODO: maybe this can be directly stored in CPU register since it must be consumed by immediate next bytecode?
    //
    TValue* m_variadicRetStart;
    uint32_t m_numVariadicRets;
    uint32_t m_unused1;

    // The linked list head of the list of open upvalues
    //
    UserHeapPointer<Upvalue> m_upvalueList;

    // The global object of this coroutine
    //
    UserHeapPointer<void> m_globalObject;

    // A temporary buffer used by DFG JIT code to pass values between JIT code and AOT slow path
    // This buffer should come before the "cold" members of this struct, so we can index this with disp8 addressing mode
    // as much as possible, which saves a bit of DFG JIT code size.
    //
    static constexpr size_t x_dfg_temp_buffer_size = 5;
    uint64_t m_dfgTempBuffer[x_dfg_temp_buffer_size];

    // The stack base of the suspend point.
    // This field is valid for all non-dead coroutine except the currently running one.
    //
    // The call frame corresponding to the stack base must be one of the following:
    // A coroutine_resume call frame, for coroutines that suspended itself by resuming another coroutine
    // A coroutine_yield call frame, for coroutines that actively suspended itself by yielding
    // A coroutine_init dummy call frame, for couroutines that have not yet started running
    //
    // The arguments passed to reenter the coroutine should always be stored starting at m_suspendPointStackBase.
    // To reenter the coroutine, one should call the return continuation of the StackFrameHeader.
    //
    TValue* m_suspendPointStackBase;

    // If this coroutine is in the stack of active coroutines (that is, !IsDead() && !IsResumable()),
    // this stores the coroutine that resumed this coroutine. For the root coroutine, this is nullptr.
    // For non-active coroutines, the value in this field is undefined.
    //
    CoroutineRuntimeContext* m_parent;

    // The beginning of the stack
    //
    TValue* m_stackBegin;
};

// Base class for some executable, either an intrinsic, or a bytecode function with some fixed global object, or a user C function
//
class ExecutableCode : public SystemHeapGcObjectHeader
{
public:
    enum class Kind : uint8_t
    {
        BytecodeFunction,
        CFunction
    };

    bool IsUserCFunction() const { return m_executableCodeKind == Kind::CFunction; }
    bool IsBytecodeFunction() const { return m_executableCodeKind == Kind::BytecodeFunction; }

    static SystemHeapPointer<ExecutableCode> WARN_UNUSED CreateCFunction(VM* vm, void* fn)
    {
        HeapPtr<ExecutableCode> e = vm->AllocFromSystemHeap(static_cast<uint32_t>(sizeof(ExecutableCode))).AsNoAssert<ExecutableCode>();
        SystemHeapGcObjectHeader::Populate(e);
        e->m_executableCodeKind = Kind::CFunction;
        e->m_hasVariadicArguments = true;
        e->m_numFixedArguments = 0;
        e->m_bestEntryPoint = fn;
        return e;
    }

    Kind m_executableCodeKind;

    // The # of fixed arguments and whether it accepts variadic arguments
    // User C function always have m_numFixedArguments == 0 and m_hasVariadicArguments == true
    //
    bool m_hasVariadicArguments;
    uint32_t m_numFixedArguments;

    struct InterpreterCallIcAnchor
    {
        InterpreterCallIcAnchor()
            : m_prevOffset(0)
            , m_nextOffset(0)
        { }

        // The prev node in the doubly linked list is 'this + m_prevOffset'
        // The next node in the doubly linked list is 'this - m_nextOffset'
        //
        int32_t m_prevOffset;
        int32_t m_nextOffset;
    };

    // For intrinsic, this is the entrypoint of the intrinsic function
    // For bytecode function, this is the most optimized implementation (interpreter or some JIT tier)
    // For user C function, this is a trampoline that calls the function
    // The 'codeBlock' parameter and 'curBytecode' parameter is not needed for intrinsic or JIT but we have them anyway for a unified interface
    //
    void* m_bestEntryPoint;

    // All interpreter call inline caches that cache on this CodeBlock, chained into a circular doubly linked list
    // Note that this is only needed for CodeBlock, as only CodeBlock needs to tier-up.
    // But we store this field unconditionally in all ExecutableCode so the interpreter does not need to check whether
    // the ExecutableCode is a CodeBlock before updating this field.
    //
    InterpreterCallIcAnchor m_interpreterCallIcList;
};
static_assert(sizeof(ExecutableCode) == 24);

class BaselineCodeBlock;

class UpvalueMetadata
{
public:
#ifndef NDEBUG
    // Whether 'm_isImmutable' field has been properly finalized, for assertion purpose only
    //
    bool m_immutabilityFieldFinalized;
#endif
    // If true, m_slot should be interpreted as the slot ordinal in parent's stack frame.
    // If false, m_slot should be interpreted as the upvalue ordinal of the parent.
    //
    bool m_isParentLocal;
    // Whether this upvalue is immutable. Currently only filled when m_isParentLocal == true.
    //
    bool m_isImmutable;
    // Where this upvalue points to.
    //
    uint32_t m_slot;
};

// Describes one entry of JIT call inline cache
//
// Each CodeBlock keeps a circular doubly-linked list of all the call IC entries caching on it,
// so that it can update the codePtr for all of them when tiering up or when the JIT code is jettisoned
// CodeBlock never invalidate any call IC. It only updates their CodePtr.
//
// If the IC is not caching on a CodeBlock (but a C function, for example), its DoublyLinkedListNode is not linked.
//
// Each call site that employs IC keeps a singly-linked list of all the IC entries it owns,
// so that it can know the cached targets, do invalidatation when transitioning from direct-call mode to closure-call mode,
// and to reclaim memory when the JIT code is jettisoned
//
// This struct always resides in the VM short-pointer data structure region
//
class JitCallInlineCacheEntry final : public SpdsDoublyLinkedListNode<JitCallInlineCacheEntry>
{
public:
    // The singly-linked list anchored at the callsite, 0 if last node
    //
    SpdsPtr<JitCallInlineCacheEntry> m_callSiteNextNode;

    // If this IC is a direct-call IC, this is the FunctionObject being cached on
    // If this IC is a closure-call IC, this is the ExecutableCode being cached on
    //
    GeneralHeapPointer<void> m_entity;

    // High 16 bits: index into the prebuilt IC trait table, to access all sorts of traits about this IC
    // Lower 48 bits: pointer to the JIT code piece
    //
    uint64_t m_taggedPtr;

    // Get the ExecutableCode of the function target cached by this IC
    //
    ExecutableCode* WARN_UNUSED GetTargetExecutableCode(VM* vm);
    ExecutableCode* WARN_UNUSED GetTargetExecutableCodeKnowingDirectCall(VM* vm);

    size_t WARN_UNUSED GetIcTraitKind()
    {
        return m_taggedPtr >> 48;
    }

    const JitCallInlineCacheTraits* WARN_UNUSED GetIcTrait()
    {
        return deegen_jit_call_inline_cache_trait_table[GetIcTraitKind()];
    }

    uint8_t* WARN_UNUSED GetJitRegionStart()
    {
        return reinterpret_cast<uint8_t*>(m_taggedPtr & ((1ULL << 48) - 1));
    }

    static JitCallInlineCacheEntry* WARN_UNUSED Create(VM* vm,
                                                       ExecutableCode* targetExecutableCode,
                                                       SpdsPtr<JitCallInlineCacheEntry> callSiteNextNode,
                                                       HeapPtr<void> entity,
                                                       size_t icTraitKind);

    // Note that this function removes the IC from the doubly-linked list (anchored at the CodeBlock) as needed,
    // but doesn't do anything about the singly-linked list (anchored at the call site)!
    //
    // So the only valid use case for this function is when a call site decides to destroy all the IC it owns.
    //
    void Destroy(VM* vm);

    // Update the target function entry point for this IC
    //
    // diff := (uint64_t)newCodePtr - (uint64_t)oldCodePtr
    //
    void UpdateTargetFunctionCodePtr(uint64_t diff)
    {
        const JitCallInlineCacheTraits* trait = GetIcTrait();
        uint8_t* jitBaseAddr = GetJitRegionStart();
        size_t numPatches = trait->m_numCodePtrUpdatePatches;
        Assert(numPatches > 0);
        size_t i = 0;
        do {
            uint8_t* addr = jitBaseAddr + trait->m_codePtrPatchRecords[i].m_offset;
            if (trait->m_codePtrPatchRecords[i].m_is64)
            {
                UnalignedStore<uint64_t>(addr, UnalignedLoad<uint64_t>(addr) + diff);
            }
            else
            {
                UnalignedStore<uint32_t>(addr, UnalignedLoad<uint32_t>(addr) + static_cast<uint32_t>(diff));
            }
            i++;
        } while (unlikely(i < numPatches));
    }
};
static_assert(sizeof(JitCallInlineCacheEntry) == 24);

// Describes one call site in JIT'ed code that employs inline caching
// Note that this must have a 1-byte alignment since this struct currently lives in the SlowPathData stream
//
struct __attribute__((__packed__, __aligned__(1))) JitCallInlineCacheSite
{
    static constexpr size_t x_maxEntries = x_maxJitCallInlineCacheEntries;

    // Try to keep this a zero initialization to avoid unnecessary work..
    //
    JitCallInlineCacheSite()
        : m_linkedListHead(SpdsPtr<JitCallInlineCacheEntry> { 0 })
        , m_numEntries(0)
        , m_mode(Mode::DirectCall)
        , m_bloomFilter(0)
    { }

    // The singly-linked list head of all the IC entries owned by this site
    //
    Packed<SpdsPtr<JitCallInlineCacheEntry>> m_linkedListHead;

    // The total number of IC entries
    //
    uint8_t m_numEntries;

    enum class Mode : uint8_t
    {
        DirectCall,
        ClosureCall,
        // When we transit from direct-call mode to closure-call mode, we invalidate all IC and start back from one IC (the target we just seen)
        // So the information of whether we have already seen more than one call targets is temporarily lost.
        // However, this information is useful for the higher tiers.
        // So we record this info here: it means even if there is only one IC entry, we actually have seen more.
        //
        ClosureCallWithMoreThanOneTargetObserved
    };

    // Whether this site is in direct-call or closure-call mode
    //
    Mode m_mode;

    // When in direct-call mode, a mini bloom filter recording all the ExecutableCode pointers cached by the ICs,
    // so we can usually rule out transition to closure-call mode without iterating through all the ICs
    //
    // With 2 hash functions, false positive rate with 3/4/5 existing items is 9.8% / 15.5% / 21.6% respectively
    //
    uint16_t m_bloomFilter;

    bool WARN_UNUSED ObservedNoTarget()
    {
        AssertImp(m_numEntries == 0, m_mode == Mode::DirectCall);
        return m_numEntries == 0;
    }

    bool WARN_UNUSED ObservedExactlyOneTarget()
    {
        return m_numEntries == 1 && m_mode != Mode::ClosureCallWithMoreThanOneTargetObserved;
    }

    // May only be called if m_numEntries < x_maxEntries and m_mode == DirectCall
    // This function handles everything except actually JIT'ting code
    //
    // Returns the address to populate JIT code
    //
    // Note that only dcIcTraitKind is passed in, because ccIcTraitKind for one IC site is always dcIcTraitKind + 1
    //
    // 'transitedToCCMode' will be written either 0 (false) or 1 (true), we use uint8_t instead of bool to avoid subtle C++ -> LLVM ABI issues
    //
    // Use attribute 'malloc' to teach LLVM that the returned address is noalias, which is very useful due to how our codegen function is written
    //
    __attribute__((__malloc__)) void* WARN_UNUSED InsertInDirectCallMode(uint16_t dcIcTraitKind, TValue tv, uint8_t* transitedToCCMode /*out*/);

    // May only be called if m_numEntries < x_maxEntries and m_mode != DirectCall
    // This function handles everything except actually JIT'ting code
    //
    // Returns the address to populate JIT code
    //
    // Note that the passed in IcTraitKind is the DC one, not the CC one!
    //
    __attribute__((__malloc__)) void* WARN_UNUSED InsertInClosureCallMode(uint16_t dcIcTraitKind, TValue tv);
};
static_assert(sizeof(JitCallInlineCacheSite) == 8);
static_assert(alignof(JitCallInlineCacheSite) == 1);

class UnlinkedCodeBlock;
class BaselineCodeBlock;
class DfgCodeBlock;

// This uniquely corresponds to each pair of <UnlinkedCodeBlock, GlobalObject>
// It owns the bytecode and the corresponding metadata (the bytecode is copied from the UnlinkedCodeBlock,
// we need our own copy because we do quickening, aka., dynamic bytecode opcode specialization optimization)
//
// Layout:
// [ constant table ] [ CodeBlock ] [ bytecode ] [ byetecode metadata ]
//
class CodeBlock final : public ExecutableCode
{
public:
    static CodeBlock* WARN_UNUSED Create(VM* vm, UnlinkedCodeBlock* ucb, UserHeapPointer<void> globalObject);

    static constexpr size_t GetTrailingArrayOffset()
    {
        return offsetof_member_v<&CodeBlock::m_bytecodeStream>;
    }

    uint8_t* GetBytecodeStream()
    {
        return reinterpret_cast<uint8_t*>(this) + GetTrailingArrayOffset();
    }

    uintptr_t GetBytecodeMetadataStart()
    {
        return reinterpret_cast<uintptr_t>(this) + GetTrailingArrayOffset() + RoundUpToMultipleOf<8>(m_bytecodeLengthIncludingTailPadding);
    }

    TValue* GetConstantTableEnd()
    {
        return reinterpret_cast<TValue*>(this);
    }

    size_t GetBytecodeLength()
    {
        return m_bytecodeLengthIncludingTailPadding - x_numExtraPaddingAtBytecodeStreamEnd;
    }

    void UpdateBestEntryPoint(void* newEntryPoint);

    UserHeapPointer<void> m_globalObject;

    uint32_t m_stackFrameNumSlots;
    uint32_t m_numUpvalues;

    // When this counter becomes negative, the function will tier up to baseline JIT
    //
    int64_t m_interpreterTierUpCounter;

    uint32_t m_bytecodeLengthIncludingTailPadding;
    uint32_t m_bytecodeMetadataLength;

    uint8_t m_fnTyMask;

    BaselineCodeBlock* m_baselineCodeBlock;
    DfgCodeBlock* m_dfgCodeBlock;

    UnlinkedCodeBlock* m_owner;

    // All JIT call inline caches that cache on this CodeBlock, chained into a circular doubly linked list
    //
    SpdsDoublyLinkedList<JitCallInlineCacheEntry> m_jitCallIcList;

    uint64_t m_bytecodeStream[0];
};

// This is just x_num_bytecode_metadata_struct_kinds
// However, unfortunately we have to make it a extern const here due to header file dependency issue...
//
extern const size_t x_num_bytecode_metadata_struct_kinds_;

namespace DeegenBytecodeBuilder { class BytecodeBuilder; }

// This uniquely corresponds to a piece of source code that defines a function
//
class UnlinkedCodeBlock : public SystemHeapGcObjectHeader
{
public:
    static UnlinkedCodeBlock* WARN_UNUSED Create(VM* vm, HeapPtr<void> /*globalObject*/)
    {
        size_t sizeToAllocate = RoundUpToMultipleOf<8>(GetTrailingArrayOffset() + x_num_bytecode_metadata_struct_kinds_ * sizeof(uint16_t));
        uint8_t* addressBegin = TranslateToRawPointer(vm, vm->AllocFromSystemHeap(static_cast<uint32_t>(sizeToAllocate)).AsNoAssert<uint8_t>());
        UnlinkedCodeBlock* ucb = reinterpret_cast<UnlinkedCodeBlock*>(addressBegin);
        ConstructInPlace(ucb);
        SystemHeapGcObjectHeader::Populate(ucb);
        ucb->m_uvFixUpCompleted = false;
        //ucb->m_defaultGlobalObject = globalObject;
        ucb->m_rareGOtoCBMap = nullptr;
        ucb->m_parent = nullptr;
        ucb->m_defaultCodeBlock = nullptr;
        ucb->m_parserUVGetFixupList = nullptr;
        ucb->m_fnKind = SOMDetailEntityType::SOM_Method;
        ucb->m_trivialFnType = SOMMethodLookupResultKind::SOM_NormalMethod;
        return ucb;
    }

    static constexpr size_t GetTrailingArrayOffset()
    {
        return offsetof_member_v<&UnlinkedCodeBlock::m_bytecodeMetadataUseCounts>;
    }

    CodeBlock* WARN_UNUSED ALWAYS_INLINE GetCodeBlock(UserHeapPointer<void> /*globalObject*/)
    {
        return m_defaultCodeBlock;
#if 0
        if (likely(globalObject == m_defaultGlobalObject))
        {
            Assert(m_defaultCodeBlock != nullptr);
            return m_defaultCodeBlock;
        }
        return GetCodeBlockSlowPath(globalObject);
#endif
    }

    CodeBlock* WARN_UNUSED NO_INLINE GetCodeBlockSlowPath(UserHeapPointer<void> globalObject)
    {
        if (unlikely(m_rareGOtoCBMap == nullptr))
        {
            m_rareGOtoCBMap = new RareGlobalObjectToCodeBlockMap;
        }
        auto iter = m_rareGOtoCBMap->find(globalObject.m_value);
        if (unlikely(iter == m_rareGOtoCBMap->end()))
        {
            VM* vm = VM::GetActiveVMForCurrentThread();
            CodeBlock* newCb = CodeBlock::Create(vm, this /*ucb*/, globalObject);
            (*m_rareGOtoCBMap)[globalObject.m_value] = newCb;
            return newCb;
        }
        else
        {
            return iter->second;
        }
    }

    void* WARN_UNUSED GetInterpreterEntryPoint();

    // For assertion purpose only
    //
    bool m_uvFixUpCompleted;
    bool m_hasVariadicArguments;
    uint32_t m_numFixedArguments;

    UserHeapPointer<void> m_defaultGlobalObject;
    CodeBlock* m_defaultCodeBlock;
    using RareGlobalObjectToCodeBlockMap = std::unordered_map<int64_t, CodeBlock*>;
    RareGlobalObjectToCodeBlockMap* m_rareGOtoCBMap;

    uint8_t* m_bytecode;
    UpvalueMetadata* m_upvalueInfo;
    // An entry in the constant table is usually a TValue, but may also be a UnlinkedCodeBlock pointer disguised as a TValue
    //
    uint64_t* m_cstTable;
    UnlinkedCodeBlock* m_parent;

    uint32_t m_bytecodeLengthIncludingTailPadding;
    uint32_t m_cstTableLength;
    uint32_t m_numUpvalues;
    uint32_t m_bytecodeMetadataLength;
    uint32_t m_stackFrameNumSlots;

    SOMDetailEntityType m_fnKind;
    SOMMethodLookupResultKind m_trivialFnType;

    std::string m_debugName;

    // Only used during parsing. Always nullptr at runtime.
    // It doesn't have to sit in this struct but the memory consumption of this struct simply shouldn't matter.
    //
    DeegenBytecodeBuilder::BytecodeBuilder* m_bytecodeBuilder;
    std::vector<uint32_t>* m_parserUVGetFixupList;

    // The actual length of this trailing array is always x_num_bytecode_metadata_struct_kinds_
    //
    uint16_t m_bytecodeMetadataUseCounts[0];
};

// Layout:
// [ constant table ] [ BaselineCodeBlock ] [ slowPathDataIndex ] [ slowPathData ]
//
// slowPathDataIndex:
//     SlowPathDataAndBytecodeOffset[N] where N is the # of bytecodes in this function.
//     Every SlowPathDataAndBytecodeOffset item records the offset of the bytecode / slowPathData in the
//     bytecode / slowPathData stream for one bytecode
// slowPathData:
//     It is similar to bytecode, but contains more information, which are needed for the JIT slow path
//     (e.g., the JIT code address to jump to if a branch is needed).
//
class alignas(8) BaselineCodeBlock
{
public:
    static BaselineCodeBlock* WARN_UNUSED Create(CodeBlock* cb,
                                                 uint32_t numBytecodes,
                                                 uint32_t slowPathDataStreamLength,
                                                 void* jitCodeEntry,
                                                 void* jitRegionStart,
                                                 uint32_t jitRegionSize);

    static constexpr size_t GetTrailingArrayOffset()
    {
        return offsetof_member_v<&BaselineCodeBlock::m_sbIndex>;
    }

    // The layout of this struct is currently hardcoded
    // If you change this, be sure to make corresponding changes in DeegenBytecodeBaselineJitInfo
    //
    struct alignas(8) SlowPathDataAndBytecodeOffset
    {
        // Note that this offset is relative to the BaselineCodeBlock pointer
        //
        uint32_t m_slowPathDataOffset;
        // This is the lower 32 bits of m_owner's bytecode pointer
        //
        uint32_t m_bytecodePtr32;
    };

    // Return the SlowPathData pointer for the given bytecode index (not bytecode offset!)
    //
    uint8_t* WARN_UNUSED GetSlowPathDataAtBytecodeIndex(size_t index)
    {
        Assert(index < m_numBytecodes);
        size_t offset = m_sbIndex[index].m_slowPathDataOffset;
        return reinterpret_cast<uint8_t*>(this) + offset;
    }

    uint8_t* WARN_UNUSED GetSlowPathDataStreamStart()
    {
        uintptr_t addr = reinterpret_cast<uintptr_t>(this);
        addr += GetTrailingArrayOffset();
        addr += sizeof(SlowPathDataAndBytecodeOffset) * m_numBytecodes;
        return reinterpret_cast<uint8_t*>(addr);
    }

    // The bytecodePtr32 must be valid.
    //
    // For now, this is simply implemented by a O(log n) binary search.
    //
    size_t WARN_UNUSED GetBytecodeIndexFromBytecodePtrLower32Bits(uint32_t bytecodePtr32)
    {
        uint32_t base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m_owner));
        uint32_t targetOffset = bytecodePtr32 - base;
        Assert(m_owner->GetTrailingArrayOffset() <= targetOffset && targetOffset < m_owner->GetBytecodeLength() + m_owner->GetTrailingArrayOffset());
        Assert(m_numBytecodes > 0);
        size_t left = 0, right = m_numBytecodes - 1;
        while (left < right)
        {
            size_t mid = (left + right) / 2;
            uint32_t value = m_sbIndex[mid].m_bytecodePtr32 - base;
            // TODO: use cmov instead of branch
            //
            if (targetOffset == value)
            {
                Assert(m_sbIndex[mid].m_bytecodePtr32 == bytecodePtr32);
                return mid;
            }
            if (targetOffset < value)
            {
                Assert(mid > 0);
                right = mid - 1;
            }
            else
            {
                left = mid + 1;
            }
        }
        Assert(left == right);
        Assert(m_sbIndex[left].m_bytecodePtr32 == bytecodePtr32);
        return left;
    }

    // The bytecodePtr must be a valid bytecode pointer in m_owner's bytecode stream
    //
    size_t WARN_UNUSED GetBytecodeIndexFromBytecodePtr(void* bytecodePtr)
    {
        Assert(m_owner->GetBytecodeStream() <= bytecodePtr && bytecodePtr < m_owner->GetBytecodeStream() + m_owner->GetBytecodeLength());
        return GetBytecodeIndexFromBytecodePtrLower32Bits(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(bytecodePtr)));
    }

    size_t WARN_UNUSED GetBytecodeOffsetFromBytecodeIndex(size_t bytecodeIndex)
    {
        Assert(bytecodeIndex < m_numBytecodes);
        uint32_t ptr32 = m_sbIndex[bytecodeIndex].m_bytecodePtr32;
        uint32_t basePtr32 = static_cast<uint32_t>(reinterpret_cast<uint64_t>(m_owner->GetBytecodeStream()));
        uint32_t diff = ptr32 - basePtr32;
        Assert(diff < m_owner->m_bytecodeLengthIncludingTailPadding);
        return diff;
    }

    UserHeapPointer<void> m_globalObject;

    uint32_t m_stackFrameNumSlots;
    uint32_t m_numBytecodes;
    // Updated by profiling logic
    //
    uint32_t m_maxObservedNumVariadicArgs;

    // Currently the JIT code is layouted as follow:
    //     [ Data Section ] [ FastPath Code ] [ SlowPath Code ]
    //
    void* m_jitCodeEntry;

    CodeBlock* m_owner;

    // The JIT region is [m_jitRegionStart, m_jitRegionStart + m_jitRegionSize)
    //
    void* m_jitRegionStart;
    uint32_t m_jitRegionSize;
    uint32_t m_slowPathDataStreamLength;

    SlowPathDataAndBytecodeOffset m_sbIndex[0];
};

// Layout:
// [ constant table ] [ DfgCodeBlock ] [ slowPathData ]
//
class alignas(8) DfgCodeBlock
{
public:
    size_t GetStackRegSpillRegionOffset()
    {
        return m_stackRegSpillRegionPhysicalSlot;
    }

    static size_t GetSlowPathDataStartOffset()
    {
        return offsetof_member_v<&DfgCodeBlock::m_slowPathData>;
    }

    UserHeapPointer<void> m_globalObject;

    uint32_t m_stackFrameNumSlots;

    // DFG stack layout:
    // [ Arguments ]: the non-vararg arguments to the function
    // [ Reg Spills ]: reserved spill locations for each register participating in reg alloc
    // [ Locals ]: storage locations for the DFG locals
    // [ Tmps ]: storage locations for spilled SSA values
    // [ Range ]: scratch space for nodes that takes a list of operands that must be placed sequentially
    //
    uint32_t m_stackRegSpillRegionPhysicalSlot;

    void* m_jitCodeEntry;

    CodeBlock* m_owner;

    // The JIT region is [m_jitRegionStart, m_jitRegionStart + m_jitRegionSize)
    //
    void* m_jitRegionStart;
    uint32_t m_jitRegionSize;
    uint32_t m_slowPathDataStreamLength;

    uint8_t m_slowPathData[0];
};

class FunctionObject;

class Upvalue
{
public:
    static HeapPtr<Upvalue> WARN_UNUSED CreateUpvalueImpl(UserHeapPointer<Upvalue> prev, TValue* dst, bool isImmutable)
    {
        VM* vm = VM::GetActiveVMForCurrentThread();
        HeapPtr<Upvalue> r = vm->AllocFromUserHeap(static_cast<uint32_t>(sizeof(Upvalue))).AsNoAssert<Upvalue>();
        UserHeapGcObjectHeader::Populate(r);
        r->m_hiddenClass.m_value = x_hiddenClassForUpvalue;
        r->m_ptr = dst;
        r->m_isClosed = false;
        r->m_isImmutable = isImmutable;
        TCSet(r->m_prev, prev);
        return r;
    }

    // Create a closed upvalue
    // Only used by DFG. By design, this will only be called for upvalues that are mutable
    //
    static Upvalue* WARN_UNUSED CreateClosed(VM* vm, TValue val)
    {
        HeapPtr<Upvalue> r = vm->AllocFromUserHeap(static_cast<uint32_t>(sizeof(Upvalue))).AsNoAssert<Upvalue>();
        Upvalue* raw = TranslateToRawPointer(vm, r);
        UserHeapGcObjectHeader::Populate(raw);
        raw->m_hiddenClass.m_value = x_hiddenClassForUpvalue;
        raw->m_ptr = &raw->m_tv;
        raw->m_tv = val;
        raw->m_isClosed = true;
        raw->m_isImmutable = false;
        return raw;
    }

    static HeapPtr<Upvalue> WARN_UNUSED Create(CoroutineRuntimeContext* rc, TValue* dst, bool isImmutable)
    {
        if (rc->m_upvalueList.m_value == 0 || rc->m_upvalueList.As()->m_ptr < dst)
        {
            // Edge case: the open upvalue list is empty, or the upvalue shall be inserted as the first element in the list
            //
            HeapPtr<Upvalue> newNode = CreateUpvalueImpl(rc->m_upvalueList /*prev*/, dst, isImmutable);
            rc->m_upvalueList = newNode;
            //WriteBarrier(rc);
            return newNode;
        }
        else
        {
            // Invariant: after the loop, the node shall be inserted between 'cur' and 'prev'
            //
            HeapPtr<Upvalue> cur = rc->m_upvalueList.As();
            TValue* curVal = cur->m_ptr;
            UserHeapPointer<Upvalue> prev;
            while (true)
            {
                Assert(!cur->m_isClosed);
                Assert(dst <= curVal);
                if (curVal == dst)
                {
                    // We found an open upvalue for that slot, we are good
                    //
                    return cur;
                }

                prev = TCGet(cur->m_prev);
                if (prev.m_value == 0)
                {
                    // 'cur' is the last node, so we found the insertion location
                    //
                    break;
                }

                Assert(!prev.As()->m_isClosed);
                TValue* prevVal = prev.As()->m_ptr;
                Assert(prevVal < curVal);
                if (prevVal < dst)
                {
                    // prevVal < dst < curVal, so we found the insertion location
                    //
                    break;
                }

                cur = prev.As();
                curVal = prevVal;
            }

            Assert(curVal == cur->m_ptr);
            Assert(prev == TCGet(cur->m_prev));
            Assert(dst < curVal);
            Assert(prev.m_value == 0 || prev.As()->m_ptr < dst);
            HeapPtr<Upvalue> newNode = CreateUpvalueImpl(prev, dst, isImmutable);
            TCSet(cur->m_prev, UserHeapPointer<Upvalue>(newNode));
            //WriteBarrier(cur);
            return newNode;
        }
    }

    void Close()
    {
        Assert(!m_isClosed);
        Assert(m_ptr != &m_tv);
        m_tv = *m_ptr;
        m_ptr = &m_tv;
        m_isClosed = true;
    }

    static constexpr int32_t x_hiddenClassForUpvalue = 0x18;

    // TODO: we could have made this structure 16 bytes instead of 32 bytes by making m_ptr a GeneralHeapPointer and takes the place of m_hiddenClass
    // (normally this is a bit risky as it might confuse all sort of things (like IC), but upvalue is so special: it is never exposed to user,
    // so an Upvalue object will never be used as operand into any bytecode instruction other than the upvalue-dedicated ones, so we are fine).
    // However, we are not doing this now because our stack is currently not placed in the VM memory range.
    //
    SystemHeapPointer<void> m_hiddenClass;
    HeapEntityType m_type;
    GcCellState m_cellState;
    // Always equal to (m_ptr == &m_u.tv)
    //
    bool m_isClosed;
    bool m_isImmutable;

    // Points to &tv for closed upvalue, or the stack slot for open upvalue
    // All the open values are chained into a linked list (through prev) in reverse sorted order of m_ptr (i.e. absolute stack slot from high to low)
    //
    TValue* m_ptr;
    // Stores the value for closed upvalue
    //
    TValue m_tv;
    // Stores the linked list if the upvalue is open
    //
    UserHeapPointer<Upvalue> m_prev;
};
static_assert(sizeof(Upvalue) == 32);

inline void __attribute__((__used__)) CoroutineRuntimeContext::CloseUpvalues(TValue* base)
{
    VM* vm = VM::GetActiveVMForCurrentThread();
    UserHeapPointer<Upvalue> cur = m_upvalueList;
    while (cur.m_value != 0)
    {
        if (cur.As()->m_ptr < base)
        {
            break;
        }
        Assert(!cur.As()->m_isClosed);
        Upvalue* uv = TranslateToRawPointer(vm, cur.As());
        cur = uv->m_prev;
        Assert(cur.m_value == 0 || cur.As()->m_ptr < uv->m_ptr);
        uv->Close();
    }
    m_upvalueList = cur;
    if (cur.m_value != 0)
    {
        //WriteBarrier(this);
    }
}

class FunctionObject
{
public:
    // Does not fill 'm_executable' or upvalue array
    //
    static UserHeapPointer<FunctionObject> WARN_UNUSED CreateImpl(VM* vm, uint8_t numUpvalues, uint8_t fnTyMask)
    {
        size_t sizeToAllocate = GetTrailingArrayOffset() + sizeof(TValue) * numUpvalues;
        sizeToAllocate = RoundUpToMultipleOf<8>(sizeToAllocate);
        HeapPtr<FunctionObject> r = vm->AllocFromUserHeap(static_cast<uint32_t>(sizeToAllocate)).AsNoAssert<FunctionObject>();
        UserHeapGcObjectHeader::Populate(r);

        r->m_numUpvalues = numUpvalues;
        r->m_invalidArrayType = fnTyMask;
        return r;
    }

    // Does not fill upvalues
    //
    static UserHeapPointer<FunctionObject> WARN_UNUSED Create(VM* vm, CodeBlock* cb)
    {
        uint32_t numUpvalues = cb->m_numUpvalues;
        Assert(numUpvalues <= std::numeric_limits<uint8_t>::max());
        UserHeapPointer<FunctionObject> r = CreateImpl(vm, static_cast<uint8_t>(numUpvalues), cb->m_fnTyMask);
        SystemHeapPointer<ExecutableCode> executable { static_cast<ExecutableCode*>(cb) };
        TCSet(r.As()->m_executable, executable);
        return r;
    }

    static UserHeapPointer<FunctionObject> WARN_UNUSED CreateCFunc(VM* vm, SystemHeapPointer<ExecutableCode> executable, uint8_t numUpvalues = 0)
    {
        constexpr uint8_t fnTyMask = static_cast<uint8_t>(static_cast<uint8_t>(SOM_Method) + static_cast<uint8_t>(SOM_NormalMethod) * 16);
        Assert(TranslateToRawPointer(executable.As())->IsUserCFunction());
        UserHeapPointer<FunctionObject> r = CreateImpl(vm, numUpvalues, fnTyMask);
        TCSet(r.As()->m_executable, executable);
        return r;
    }

    // DEVNOTE: C library function must not use this function.
    //
    static bool ALWAYS_INLINE IsUpvalueImmutable(HeapPtr<FunctionObject> self, size_t ord)
    {
        Assert(ord < self->m_numUpvalues);
        Assert(TranslateToRawPointer(TCGet(self->m_executable).As())->IsBytecodeFunction());
        HeapPtr<CodeBlock> cb = static_cast<HeapPtr<CodeBlock>>(TCGet(self->m_executable).As());
        Assert(cb->m_numUpvalues == self->m_numUpvalues && cb->m_owner->m_numUpvalues == self->m_numUpvalues);
        Assert(cb->m_owner->m_upvalueInfo[ord].m_immutabilityFieldFinalized);
        return cb->m_owner->m_upvalueInfo[ord].m_isImmutable;
    }

    // Get the upvalue ptr for a mutable upvalue
    //
    // DEVNOTE: C library function must not use this function.
    //
    static Upvalue* ALWAYS_INLINE GetMutableUpvaluePtr(HeapPtr<FunctionObject> self, size_t ord)
    {
        Assert(ord < self->m_numUpvalues);
        Assert(!IsUpvalueImmutable(self, ord));
        TValue tv = TCGet(self->m_upvalues[ord]);
        Upvalue* uvPtr = reinterpret_cast<Upvalue*>(tv.m_value);
        Assert(uvPtr->m_type == HeapEntityType::Upvalue);
        return uvPtr;
    }

    // Get the value of an immutable upvalue
    //
    // DEVNOTE: C library function must not use this function.
    //
    static TValue ALWAYS_INLINE GetImmutableUpvalueValue(HeapPtr<FunctionObject> self, size_t ord)
    {
        Assert(ord < self->m_numUpvalues);
        Assert(IsUpvalueImmutable(self, ord));
        TValue tv = TCGet(self->m_upvalues[ord]);
        Assert(!(tv.IsPointer() && tv.GetHeapEntityType() == HeapEntityType::Upvalue));
        return tv;
    }

    // Get the value of an upvalue, works no matter if the upvalue is mutable or immutable.
    // Of course, this is also (quite) slow.
    //
    // DEVNOTE: C library function must not use this function.
    //
    static TValue ALWAYS_INLINE GetUpvalueValue(HeapPtr<FunctionObject> self, size_t ord)
    {
        Assert(ord < self->m_numUpvalues);
        if (IsUpvalueImmutable(self, ord))
        {
            return GetImmutableUpvalueValue(self, ord);
        }
        else
        {
            Upvalue* uv = GetMutableUpvaluePtr(self, ord);
            return *uv->m_ptr;
        }
    }

    static TValue GetMutableUpvaluePtrOrImmutableUpvalue(HeapPtr<FunctionObject> self, size_t ord)
    {
        Assert(ord < self->m_numUpvalues);
        return TCGet(self->m_upvalues[ord]);
    }

    static UserHeapPointer<FunctionObject> WARN_UNUSED NO_INLINE CreateAndFillUpvalues(
        CodeBlock* cb, CoroutineRuntimeContext* rc, TValue* stackFrameBase, HeapPtr<FunctionObject> parent, size_t selfOrdinalInStackFrame);

    // Create the function object, and fill in all upvalues that come from parent upvalues (i.e., enclosing function's upvalues)
    //
    // All upvalues that comes from the enclosing function's local variables are not populated
    //
    static FunctionObject* WARN_UNUSED NO_INLINE CreateForDfgAndFillUpvaluesFromParent(
        UnlinkedCodeBlock* ucb, HeapPtr<FunctionObject> parent);

    static constexpr size_t GetTrailingArrayOffset()
    {
        return offsetof_member_v<&FunctionObject::m_upvalues>;
    }

    // Return the byte offset of the address for upvalue ordinal 'uvOrd' from the base address of this object
    //
    static constexpr size_t GetUpvalueAddrByteOffsetFromThisPointer(size_t uvOrd)
    {
        return GetTrailingArrayOffset() + sizeof(TValue) * uvOrd;
    }

    // Right now we are storing m_numUpvalues in a uint8_t
    //
    static constexpr size_t x_maxNumUpvalues = 255;

    // Object header
    //
    // Note that a CodeBlock defines both UnlinkedCodeBlock and GlobalObject,
    // so the upvalue list does not contain the global object (if the ExecutableCode is not a CodeBlock, then the global object doesn't matter either)
    //
    SystemHeapPointer<ExecutableCode> m_executable;
    HeapEntityType m_type;
    GcCellState m_cellState;

    uint8_t m_numUpvalues;
    // Always ArrayType::x_invalidArrayType
    //
    uint8_t m_invalidArrayType;

    // The upvalue list.
    // The interpretation of each element in the list depends on whether the upvalue is immutable
    // (this information is recorded in the UnlinkedCodeBlock's upvalue metadata list):
    //
    // 1. If the upvalue is not immutable, then the TValue must be a Upvalue* object (disguised as a TValue),
    //    and the value of the upvalue should be read from the Upvalue object.
    // 2. If the upvalue is immutable, then the TValue must not be a HeapPtr<Upvalue> (since upvalue
    //    objects are never exposed directly to user code). The TValue itself is simply the value of the upvalue.
    //
    TValue m_upvalues[0];
};
static_assert(sizeof(FunctionObject) == 8);

inline ExecutableCode* WARN_UNUSED JitCallInlineCacheEntry::GetTargetExecutableCode(VM* vm)
{
    AssertIff(m_entity.IsUserHeapPointer(), GetIcTrait()->m_isDirectCallMode);
    ExecutableCode* ec;
    if (m_entity.IsUserHeapPointer())
    {
        Assert(m_entity.As<UserHeapGcObjectHeader>()->m_type == HeapEntityType::Function);
        ec = TranslateToRawPointer(vm, TCGet(m_entity.As<FunctionObject>()->m_executable).As());
    }
    else
    {
        Assert(m_entity.As<SystemHeapGcObjectHeader>()->m_type == HeapEntityType::ExecutableCode);
        ec = TranslateToRawPointer(vm, m_entity.As<ExecutableCode>());
    }
    AssertIff(IsOnDoublyLinkedList(this), ec->IsBytecodeFunction());
    return ec;
}

inline ExecutableCode* WARN_UNUSED JitCallInlineCacheEntry::GetTargetExecutableCodeKnowingDirectCall(VM* vm)
{
    Assert(GetIcTrait()->m_isDirectCallMode);
    Assert(m_entity.IsUserHeapPointer());
    Assert(m_entity.As<UserHeapGcObjectHeader>()->m_type == HeapEntityType::Function);
    ExecutableCode* ec = TranslateToRawPointer(vm, TCGet(m_entity.As<FunctionObject>()->m_executable).As());
    AssertIff(IsOnDoublyLinkedList(this), ec->IsBytecodeFunction());
    return ec;
}

// Corresponds to a file
//
class ScriptModule
{
public:
    std::string m_name;
    std::vector<UnlinkedCodeBlock*> m_unlinkedCodeBlocks;
    UserHeapPointer<void> m_defaultGlobalObject;
    UserHeapPointer<FunctionObject> m_defaultEntryPoint;

    static std::unique_ptr<ScriptModule> WARN_UNUSED LegacyParseScriptFromJSONBytecodeDump(VM* vm, UserHeapPointer<void> globalObject, const std::string& content);
};

inline void PrintTValue(FILE* fp, TValue val)
{
    if (val.IsInt32())
    {
        fprintf(fp, "%d", static_cast<int>(val.AsInt32()));
    }
    else if (val.IsDouble())
    {
        double dbl = val.AsDouble();
        fprintf(fp, "%lf", dbl);
    }
    else if (val.IsMIV())
    {
        MiscImmediateValue miv = val.AsMIV();
        if (miv.IsNil())
        {
            fprintf(fp, "nil");
        }
        else
        {
            Assert(miv.IsBoolean());
            fprintf(fp, "%s", (miv.GetBooleanValue() ? "true" : "false"));
        }
    }
    else
    {
        Assert(val.IsPointer());
        UserHeapGcObjectHeader* p = TranslateToRawPointer(val.AsPointer<UserHeapGcObjectHeader>().As());
        fprintf(fp, "(type %d)", static_cast<int>(p->m_type));
        fprintf(fp, ": %p", static_cast<void*>(p));
    }
}
