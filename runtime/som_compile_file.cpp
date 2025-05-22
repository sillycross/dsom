#include "som_compile_file.h"
#include "som_parser.h"
#include "som_class.h"
#include "vm.h"
#include "runtime_utils.h"
#include <fstream>
#include "bytecode_builder.h"
#include "tvalue.h"

using namespace DeegenBytecodeBuilder;

std::vector<std::string> g_classLoadPaths = { };

static std::ifstream WARN_UNUSED OpenFileForClass(std::string className)
{
    for (const std::string& path : g_classLoadPaths)
    {
        std::string filename = path + "/" + className + ".som";
        std::ifstream fp{};
        fp.open(filename.c_str(), std::ios_base::in);
        if (fp.is_open())
        {
            return fp;
        }
    }
    fprintf(stderr, "Failed to load class %s (file not found)\n", className.c_str());
    abort();
}

void SetSOMGlobal(VM* vm, std::string_view key, TValue value)
{
    size_t slot = vm->GetSlotForGlobal(key);
    TestAssert(vm->m_somGlobals[slot].m_value == TValue::CreateImpossibleValue().m_value);
    vm->m_somGlobals[slot] = value;
}

SOMUniquedString GetUniquedString(VM* vm, std::string_view str)
{
    return vm->GetUniquedString(str);
}

HeapPtr<FunctionObject> SOMGetMethodFromClass(SOMClass* c, std::string_view meth)
{
    VM* vm = VM_GetActiveVMForCurrentThread();
    SOMUniquedString s = GetUniquedString(vm, meth);
    GeneralHeapPointer<FunctionObject> res = SOMClass::GetMethod(TranslateToHeapPtr(c), s);
    if (res.m_value == 0) { return nullptr; }
    return res.As();
}

struct BlockTranslationContext;

struct LocalVarInfo
{
    LocalVarInfo(BlockTranslationContext* bctx, uint32_t slotOrd)
        : m_bctx(bctx)
        , m_slotOrd(slotOrd)
        , m_isUsed(false)
        , m_isWritten(false)
        , m_isImmutable(true)
    { }

    // Where it is defined
    //
    BlockTranslationContext* m_bctx;
    uint32_t m_slotOrd;
    // If all writes to the variable happens before all uses to the variable, and all writes are in the block where it is defined,
    // it can be treated as immutable for upvalue purpose
    //
    bool m_isUsed;
    bool m_isWritten;
    bool m_isImmutable;

    void NotifyUse()
    {
        m_isUsed = true;
    }

    void NotifyWrite(bool isInDefiningBlock)
    {
        if (m_isUsed || !isInDefiningBlock)
        {
            m_isImmutable = false;
        }
        m_isWritten = true;
    }
};

struct TranslationContext
{
    TranslationContext(TempArenaAllocator& alloc,
                       TempUnorderedMap<std::string_view, TempVector<LocalVarInfo*>>& localMap,
                       TempUnorderedMap<std::string_view, uint32_t /*objectSlotOrd*/>& fieldMap,
                       bool isSelfObject,
                       SOMClass* superClass,
                       TempVector<TranslationContext*>& resultTcs)
        : m_alloc(alloc)
        , m_localVarMap(localMap)
        , m_fieldMap(fieldMap)
        , m_historyTopSlot(0)
        , m_isSelfObject(isSelfObject)
        , m_superClass(superClass)
        , m_builder()
        , m_allUpvalueGetBytecodes(alloc)
        , m_results(resultTcs)
        , m_resultUcb(nullptr)
        , m_resultBCtx(nullptr)
    {
        m_results.push_back(this);
        m_resultUcb = UnlinkedCodeBlock::Create(VM_GetActiveVMForCurrentThread(), nullptr);
    }

    void UpdateTopSlot(uint32_t slot)
    {
        m_historyTopSlot = std::max(m_historyTopSlot, slot);
    }

    TempArenaAllocator& m_alloc;
    // m_localVarMap[name].back() is the slot for local var "name"
    //
    TempUnorderedMap<std::string_view, TempVector<LocalVarInfo*>>& m_localVarMap;
    TempUnorderedMap<std::string_view, uint32_t /*objectSlotOrd*/>& m_fieldMap;
    uint32_t m_historyTopSlot;
    // True if 'self' is known to be an SOMObject (that is, not nil/bool/int/double/block/method)
    //
    bool m_isSelfObject;
    SOMClass* m_superClass;
    BytecodeBuilder m_builder;
    TempVector<size_t> m_allUpvalueGetBytecodes;
    TempVector<TranslationContext*>& m_results;
    UnlinkedCodeBlock* m_resultUcb;
    BlockTranslationContext* m_resultBCtx;
};

struct UVInfo
{
    bool m_isParentLocal;
    uint32_t m_parentLocalSlot;
    uint32_t m_parentUpvalueOrd;
    LocalVarInfo* m_varInfo;
};

struct BlockTranslationContext
{
    BlockTranslationContext(TempArenaAllocator& alloc,
                            BlockTranslationContext* lexicalParent,
                            BlockTranslationContext* trueParent)
        : m_startSlot(0)
        , m_lexicalParent(lexicalParent)
        , m_owningContext(this)
        , m_trueParent(trueParent)
        , m_hasCapturedLocal(false)
        , m_containsThrowableBlocks(false)
        , m_blockMayThrow(false)
        , m_returnValueMayBeDiscarded(false)
        , m_returnValueSlot(static_cast<uint32_t>(-1))
        , m_uvs(alloc)
        , m_uvOrdMap(alloc)
    { }

    BlockTranslationContext(TempArenaAllocator& alloc,
                            uint32_t startSlot,
                            BlockTranslationContext* lexicalParent,
                            BlockTranslationContext* owningContext,
                            BlockTranslationContext* trueParent)
        : m_startSlot(startSlot)
        , m_lexicalParent(lexicalParent)
        , m_owningContext(owningContext)
        , m_trueParent(trueParent)
        , m_hasCapturedLocal(false)
        , m_containsThrowableBlocks(false)
        , m_blockMayThrow(false)
        , m_returnValueMayBeDiscarded(false)
        , m_returnValueSlot(static_cast<uint32_t>(-1))
        , m_uvs(alloc)
        , m_uvOrdMap(alloc)
    { }

    bool IsInlined()
    {
        return m_owningContext != this;
    }

    // Return true if this block is either a top-level method, or a block inlined into a top-level method
    //
    bool IsInlinedIntoTopLevelMethod()
    {
        return m_trueParent == nullptr;
    }

    // Return true if this block is the top-level method itself
    //
    bool IsTopLevelMethod()
    {
        return m_trueParent == nullptr && m_owningContext == this;
    }

    // For blocks inlined into a top-level method, the throw expession should be compiled to return
    // In other cases, it should still be compiled to a throw
    //
    bool ShouldCompileThrowIntoReturn()
    {
        // Throw doesn't make sense in top-level method
        //
        TestAssert(!IsTopLevelMethod());
        return IsInlinedIntoTopLevelMethod();
    }

    // For blocks inined into another block, the return value is sometimes discardable.
    //
    bool ReturnValueMayBeDiscarded()
    {
        TestAssert(IsInlined());
        return m_returnValueMayBeDiscarded;
    }

    uint32_t GetReturnValueSlotForInlinedBlock()
    {
        TestAssert(IsInlined());
        TestAssert(m_returnValueSlot != static_cast<uint32_t>(-1));
        return m_returnValueSlot;
    }

    // Return the upvalue ordinal
    //
    size_t WARN_UNUSED AddUpvalue(LocalVarInfo* var)
    {
        TestAssert(!IsInlined());
        var->m_bctx->m_hasCapturedLocal = true;
        BlockTranslationContext* defContext = var->m_bctx->m_owningContext;
        TestAssert(this != defContext);
        TestAssert(m_trueParent != nullptr);
        {
            auto it = m_uvOrdMap.find(var);
            if (it != m_uvOrdMap.end())
            {
                return it->second;
            }
        }
        if (m_trueParent == defContext)
        {
            m_uvs.push_back(UVInfo {
                .m_isParentLocal = true,
                .m_parentLocalSlot = var->m_slotOrd,
                .m_parentUpvalueOrd = static_cast<uint32_t>(-1),
                .m_varInfo = var
            });
        }
        else
        {
            size_t parentUvOrd = m_trueParent->AddUpvalue(var);
            m_uvs.push_back(UVInfo {
                .m_isParentLocal = false,
                .m_parentLocalSlot = static_cast<uint32_t>(-1),
                .m_parentUpvalueOrd = static_cast<uint32_t>(parentUvOrd),
                .m_varInfo = var
            });
        }
        m_uvOrdMap[var] = m_uvs.size() - 1;
        return m_uvs.size() - 1;
    }

    bool MustEmitUpvalueCloseBeforeReturn()
    {
        return m_hasCapturedLocal || m_containsThrowableBlocks;
    }

    bool MustMutablyCaptureSelf()
    {
        return m_containsThrowableBlocks || m_blockMayThrow;
    }

    // The base slot of this function
    //
    uint32_t m_startSlot;
    // Say A -> [inlined B] -> [inlined C] -> D -> [inlined E] -> [inlined F] where we are [inlined F],
    // then m_lexicalParent is E, m_owningContext is D, m_trueParent is A
    //
    BlockTranslationContext* m_lexicalParent;
    BlockTranslationContext* m_owningContext;
    BlockTranslationContext* m_trueParent;
    // True if this block contains locals *excluding self* that gets captured
    //
    bool m_hasCapturedLocal;
    // True if this block may create or transitively create blocks that contain a throw expression.
    // If this or m_hasCapturedLocal is true, the block must create UpvalueClose before it returns
    //
    bool m_containsThrowableBlocks;
    // True if this block itself contains a throw expression.
    // Note that for blocks inlined into top-level, throw becomes return, so this variable will be false in such cases.
    // If this or m_containsThrowableBlocks is true, then this block must mutably capture 'self'
    //
    bool m_blockMayThrow;
    // Only makes sense if this block is inlined
    // In this case, the return value should be stored into a slot instead, and sometimes is discardable
    //
    bool m_returnValueMayBeDiscarded;
    uint32_t m_returnValueSlot;
    // Only valid if !IsInlined()
    // Stores the list of upvalues used by this block
    // For any block, m_uvs[0] is always the capture 'self'
    //
    TempVector<UVInfo> m_uvs;
    TempUnorderedMap<LocalVarInfo*, size_t> m_uvOrdMap;
};

TValue WARN_UNUSED CreateConstantNonArray(AstLiteral* node)
{
    TestAssert(node->IsNonArrayLiteral());
    switch (node->GetKind())
    {
    case AstExprKind::Double:
    {
        AstDouble* val = assert_cast<AstDouble*>(node);
        return TValue::Create<tDouble>(val->m_value);
    }
    case AstExprKind::Integer:
    {
        AstInteger* val = assert_cast<AstInteger*>(node);
        return TValue::Create<tInt32>(val->m_value);
    }
    case AstExprKind::String:
    {
        AstString* val = assert_cast<AstString*>(node);
        VM* vm = VM_GetActiveVMForCurrentThread();
        return TValue::Create<tObject>(TranslateToHeapPtr(vm->GetInternedString(val->m_globalOrd)));
    }
    case AstExprKind::Symbol:
    {
        AstSymbol* val = assert_cast<AstSymbol*>(node);
        VM* vm = VM_GetActiveVMForCurrentThread();
        return TValue::Create<tObject>(TranslateToHeapPtr(vm->GetInternedSymbol(val->m_globalOrd)));
    }
    default:
    {
        TestAssert(false);
        __builtin_unreachable();
    }
    }
}

TValue WARN_UNUSED CreateConstantArray(AstArray* arr)
{
    SOMObject* r = SOMObject::AllocateArray(arr->m_elements.size());
    for (size_t i = 0; i < arr->m_elements.size(); i++)
    {
        AstLiteral* element = arr->m_elements[i];
        if (element->GetKind() == AstExprKind::Array)
        {
            r->m_data[i + 1] = CreateConstantArray(assert_cast<AstArray*>(element));
        }
        else
        {
            r->m_data[i + 1] = CreateConstantNonArray(element);
        }
    }
    return TValue::Create<tObject>(TranslateToHeapPtr(r));
}

// The invariant for 'clobberSlot/destSlot' of these "compile" functions is:
// the function may generate bytecode that clobbers anything >= clobberSlot, and the result is stored in destSlot at the end
//
void CompileExpression(TranslationContext& ctx, BlockTranslationContext& bctx, AstExpr* node, uint32_t clobberSlot, uint32_t destSlot);

void CompileAstLiteral(TranslationContext& ctx, BlockTranslationContext& /*bctx*/, AstLiteral* node, uint32_t /*clobberSlot*/, uint32_t destSlot)
{
    ctx.UpdateTopSlot(destSlot);
    TestAssert(node->IsLiteral());
    if (node->GetKind() == AstExprKind::Array)
    {
        TValue tv = CreateConstantArray(assert_cast<AstArray*>(node));
        ctx.m_builder.CreateSOMArrayDup({
            .src = tv,
            .output = Local(destSlot)
        });
    }
    else
    {
        TValue tv = CreateConstantNonArray(node);
        ctx.m_builder.CreateMov({
            .input = tv,
            .output = Local(destSlot)
        });
    }
}

enum class VarUseKind
{
    // The variable resolves to a local
    //
    Local,
    // The variable resolves to an upvalue
    //
    Upvalue,
    // The variable resolves to a field of self
    //
    Field,
    // The variable does not resolve to anything above, so it's a global read or invalid write
    //
    Global,
    // The variable resolves to global 'true'/'false'/'nil',
    // which are assumed by SOM++/TruffleSOM to always hold the respective constant values
    //
    FalseTrueNil
};

struct VarResolveResult
{
    VarUseKind m_kind;
    // For m_kind == FalseTrueNil, m_ord = 0 means false, m_ord = 1 means true, m_ord = 2 means nil
    //
    uint32_t m_ord;

    static VarResolveResult GetForFalse() { return { .m_kind = VarUseKind::FalseTrueNil, .m_ord = 0 }; }
    static VarResolveResult GetForTrue() { return { .m_kind = VarUseKind::FalseTrueNil, .m_ord = 1 }; }
    static VarResolveResult GetForNil() { return { .m_kind = VarUseKind::FalseTrueNil, .m_ord = 2 }; }

    TValue GetTValueForFalseTrueNil()
    {
        TestAssert(m_kind == VarUseKind::FalseTrueNil);
        if (m_ord == 0) { return TValue::Create<tBool>(false); }
        if (m_ord == 1) { return TValue::Create<tBool>(true); }
        TestAssert(m_ord == 2);
        return TValue::Create<tNil>();
    }
};

enum class VarResolveMode
{
    ForRead,
    ForWrite,
    // Does not record a read or write for the variable
    //
    DryRun
};

VarResolveResult WARN_UNUSED ResolveVariable(TranslationContext& ctx, BlockTranslationContext& bctx, std::string_view varName, VarResolveMode mode)
{
    if (varName == "self" || varName == "super")
    {
        if (mode == VarResolveMode::ForWrite)
        {
            fprintf(stderr, "Write to 'self' or 'super' is not allowed!\n");
            abort();
        }
        // The value of 'self' is always explicitly passed in as argument 0, which sits at local 0
        // 'super' is same as 'self' when used as a variable (it only has special semantics for calls)
        //
        return VarResolveResult {
            .m_kind = VarUseKind::Local,
            .m_ord = 0
        };
    }
    {
        auto it = ctx.m_localVarMap.find(varName);
        if (it != ctx.m_localVarMap.end() && !it->second.empty())
        {
            LocalVarInfo* var = it->second.back();
            if (mode == VarResolveMode::ForRead)
            {
                var->NotifyUse();
            }
            else if (mode == VarResolveMode::ForWrite)
            {
                var->NotifyWrite(&bctx == var->m_bctx /*isInDefiningBlock*/);
            }
            if (var->m_bctx->m_owningContext != bctx.m_owningContext)
            {
                // This is a variable defined in an outer function
                //
                TestAssertImp(mode == VarResolveMode::ForWrite, !var->m_isImmutable);
                size_t uvOrd = bctx.m_owningContext->AddUpvalue(var);
                return VarResolveResult {
                    .m_kind = VarUseKind::Upvalue,
                    .m_ord = static_cast<uint32_t>(uvOrd)
                };
            }
            else
            {
                // This is a variable in the current function
                //
                return VarResolveResult {
                    .m_kind = VarUseKind::Local,
                    .m_ord = var->m_slotOrd
                };
            }
        }
    }
    {
        auto it = ctx.m_fieldMap.find(varName);
        if (it != ctx.m_fieldMap.end())
        {
            return VarResolveResult {
                .m_kind = VarUseKind::Field,
                .m_ord = it->second
            };
        }
    }
    if (mode != VarResolveMode::ForWrite)
    {
        if (varName == "false") { return VarResolveResult::GetForFalse(); }
        if (varName == "true") { return VarResolveResult::GetForTrue(); }
        if (varName == "nil") { return VarResolveResult::GetForNil(); }
        VM* vm = VM_GetActiveVMForCurrentThread();
        size_t slotForGlobal = vm->GetSlotForGlobal(varName);
        return VarResolveResult {
            .m_kind = VarUseKind::Global,
            .m_ord = static_cast<uint32_t>(slotForGlobal)
        };
    }
    else
    {
        fprintf(stderr, "Write to undefined variable/field %s is not allowed!\n", std::string(varName).c_str());
        abort();
    }
}

bool WARN_UNUSED IsVariableResolvedToLocal(TranslationContext& ctx, BlockTranslationContext& bctx, std::string_view varName)
{
    VarResolveResult vr = ResolveVariable(ctx, bctx, varName, VarResolveMode::DryRun);
    return vr.m_kind == VarUseKind::Local;
}

bool WARN_UNUSED IsVariableResolvedToGlobal(TranslationContext& ctx, BlockTranslationContext& bctx, std::string_view varName)
{
    VarResolveResult vr = ResolveVariable(ctx, bctx, varName, VarResolveMode::DryRun);
    return vr.m_kind == VarUseKind::Global;
}

bool WARN_UNUSED IsVariableResolvedToFalseTrueNil(TranslationContext& ctx, BlockTranslationContext& bctx, std::string_view varName)
{
    VarResolveResult vr = ResolveVariable(ctx, bctx, varName, VarResolveMode::DryRun);
    return vr.m_kind == VarUseKind::FalseTrueNil;
}

bool WARN_UNUSED IsVariableResolvedToLocalOrFalseTrueNil(TranslationContext& ctx, BlockTranslationContext& bctx, std::string_view varName)
{
    VarResolveResult vr = ResolveVariable(ctx, bctx, varName, VarResolveMode::DryRun);
    return vr.m_kind == VarUseKind::Local || vr.m_kind == VarUseKind::FalseTrueNil;
}

// Return true if 'expr' is a VariableUse that resolves to a local.
// In which case, this function returns true, a read to the local is registered, and 'slot' will be stored the local variable ordinal
//
bool WARN_UNUSED DetectTrivialLocalVarUse(TranslationContext& ctx, BlockTranslationContext& bctx, AstExpr* expr, uint32_t& slot /*output*/)
{
    if (expr->GetKind() == AstExprKind::VarUse &&
        IsVariableResolvedToLocal(ctx, bctx, assert_cast<AstVariableUse*>(expr)->m_varInfo.m_name))
    {
        VarResolveResult vr = ResolveVariable(ctx, bctx, assert_cast<AstVariableUse*>(expr)->m_varInfo.m_name, VarResolveMode::ForRead);
        TestAssert(vr.m_kind == VarUseKind::Local);
        slot = vr.m_ord;
        return true;
    }
    return false;
}

// Return true if 'expr' is a non-array constant, a VariableUse that resolves to a local,
// or a VariableUse that resolves to a special global (true/false/nil)
// In these cases, this function returns true, a read to the local is registered, and 'res' stores the information of the local or constant
//
bool WARN_UNUSED DetectTrivialLocalVarOrConstantUse(TranslationContext& ctx, BlockTranslationContext& bctx, AstExpr* expr, LocalOrCstWrapper& res /*out*/)
{
    if (expr->IsNonArrayLiteral())
    {
        res = CreateConstantNonArray(assert_cast<AstLiteral*>(expr));
        return true;
    }

    if (expr->GetKind() == AstExprKind::VarUse &&
        IsVariableResolvedToLocalOrFalseTrueNil(ctx, bctx, assert_cast<AstVariableUse*>(expr)->m_varInfo.m_name))
    {
        VarResolveResult vr = ResolveVariable(ctx, bctx, assert_cast<AstVariableUse*>(expr)->m_varInfo.m_name, VarResolveMode::ForRead);
        if (vr.m_kind == VarUseKind::Local)
        {
            res = Local(vr.m_ord);
        }
        else
        {
            TestAssert(vr.m_kind == VarUseKind::FalseTrueNil);
            res = vr.GetTValueForFalseTrueNil();
        }
        return true;
    }
    return false;
}

void CompileVarUse(TranslationContext& ctx, BlockTranslationContext& bctx, AstVariableUse* node, uint32_t /*clobberSlot*/, uint32_t destSlot)
{
    ctx.UpdateTopSlot(destSlot);
    VarResolveResult vr = ResolveVariable(ctx, bctx, node->m_varInfo.m_name, VarResolveMode::ForRead);
    if (vr.m_kind == VarUseKind::Local)
    {
        ctx.m_builder.CreateMov({
            .input = Local(vr.m_ord),
            .output = Local(destSlot)
        });
    }
    else if (vr.m_kind == VarUseKind::Upvalue)
    {
        // May be changed to GetImmutable later
        //
        ctx.m_allUpvalueGetBytecodes.push_back(ctx.m_builder.GetCurLength());
        ctx.m_builder.CreateUpvalueGetMutable({
            .ord = SafeIntegerCast<uint16_t>(vr.m_ord),
            .output = Local(destSlot)
        });
    }
    else if (vr.m_kind == VarUseKind::Field)
    {
        ctx.m_builder.CreateSOMGetField({
            .base = Local(0),
            .index = SafeIntegerCast<uint16_t>(vr.m_ord),
            .output = Local(destSlot)
        });
    }
    else if (vr.m_kind == VarUseKind::Global)
    {
#ifdef TESTBUILD
        VM* vm = VM_GetActiveVMForCurrentThread();
        TestAssert(vr.m_ord < vm->m_globalStringIdWithIndex.size());
        std::string_view globalName = vm->m_interner.Get(vm->m_globalStringIdWithIndex[vr.m_ord]);
        TestAssert(globalName != "false" && globalName != "true" && globalName != "nil");
#endif
        ctx.m_builder.CreateSOMGlobalGet({
            .self = Local(0),
            .index = SafeIntegerCast<uint16_t>(vr.m_ord),
            .output = Local(destSlot)
        });
    }
    else
    {
        TestAssert(vr.m_kind == VarUseKind::FalseTrueNil);
        ctx.m_builder.CreateMov({
            .input = vr.GetTValueForFalseTrueNil(),
            .output = Local(destSlot)
        });
    }
}

void EmitAssignValueToLhs(TranslationContext& ctx, VarResolveResult vr, LocalOrCstWrapper valToPut)
{
    if (vr.m_kind == VarUseKind::Local)
    {
        ctx.m_builder.CreateMov({
            .input = valToPut,
            .output = Local(vr.m_ord)
        });
    }
    else if (vr.m_kind == VarUseKind::Upvalue)
    {
        ctx.m_builder.CreateUpvaluePut({
            .ord = SafeIntegerCast<uint16_t>(vr.m_ord),
            .value = valToPut
        });
    }
    else if (vr.m_kind == VarUseKind::Field)
    {
        ctx.m_builder.CreateSOMPutField({
            .base = Local(0),
            .index = SafeIntegerCast<uint16_t>(vr.m_ord),
            .value = valToPut
        });
    }
    else
    {
        TestAssert(false && "put global should not happen here!");
        __builtin_unreachable();
    }
}

void CompileAssignation(TranslationContext& ctx, BlockTranslationContext& bctx, AstAssignation* node, uint32_t clobberSlot, uint32_t destSlot)
{
    ctx.UpdateTopSlot(destSlot);
    CompileExpression(ctx, bctx, node->m_rhs, clobberSlot, destSlot);
    Local valToPut = Local(destSlot);
    for (VariableInfo& vi : node->m_lhs)
    {
        VarResolveResult vr = ResolveVariable(ctx, bctx, vi.m_name, VarResolveMode::ForWrite);
        EmitAssignValueToLhs(ctx, vr, valToPut);
    }
}

// Compile a top-level statement that is an assignations
//
void CompileTopLevelAssignation(TranslationContext& ctx, BlockTranslationContext& bctx, AstAssignation* node, uint32_t clobberSlot)
{
    // If rhs is a local variable or a non-array literal, we can always directly move it into lhs
    //
    {
        LocalOrCstWrapper rhs = Local(0);
        if (DetectTrivialLocalVarOrConstantUse(ctx, bctx, node->m_rhs, rhs /*out*/))
        {
            for (VariableInfo& vi : node->m_lhs)
            {
                VarResolveResult vr = ResolveVariable(ctx, bctx, vi.m_name, VarResolveMode::ForWrite);
                EmitAssignValueToLhs(ctx, vr, rhs);
            }
            return;
        }
    }

    {
        // If one of the lhs is a local, we can evaluate the varUse into that local, and copy the value to all other lhs
        //
        size_t lhsLocalOrd = static_cast<size_t>(-1);
        for (size_t i = 0; i < node->m_lhs.size(); i++)
        {
            if (IsVariableResolvedToLocal(ctx, bctx, node->m_lhs[i].m_name))
            {
                lhsLocalOrd = i;
                break;
            }
        }
        if (lhsLocalOrd != static_cast<size_t>(-1))
        {
            VarResolveResult vr = ResolveVariable(ctx, bctx, node->m_lhs[lhsLocalOrd].m_name, VarResolveMode::ForWrite);
            TestAssert(vr.m_kind == VarUseKind::Local);
            uint32_t destSlot = vr.m_ord;
            Local valToPut = Local(destSlot);
            CompileExpression(ctx, bctx, node->m_rhs, clobberSlot, destSlot);
            for (size_t i = 0; i < node->m_lhs.size(); i++)
            {
                if (i == lhsLocalOrd)
                {
                    continue;
                }
                vr = ResolveVariable(ctx, bctx, node->m_lhs[i].m_name, VarResolveMode::ForWrite);
                EmitAssignValueToLhs(ctx, vr, valToPut);
            }
            return;
        }
    }

    CompileAssignation(ctx, bctx, node, clobberSlot, clobberSlot /*destSlot*/);
}

void InlineBlock(TranslationContext& ctx,
                 BlockTranslationContext& bctx,
                 AstNestedBlock* block,
                 uint32_t frameStartSlot,
                 uint32_t destSlot,
                 bool mayDiscardReturnValue);

bool WARN_UNUSED ReceiverIsSelf(AstExpr* e)
{
    return (e->GetKind() == AstExprKind::VarUse && assert_cast<AstVariableUse*>(e)->m_varInfo.m_name == "self");
}

bool WARN_UNUSED ReceiverIsSuper(AstExpr* e)
{
    return (e->GetKind() == AstExprKind::VarUse && assert_cast<AstVariableUse*>(e)->m_varInfo.m_name == "super");
}

enum class SOMReceiverKind
{
    Normal,
    // The receiver is 'self' and is an SOMObject
    //
    ObjectSelf,
    // The receiver is 'super'. Coincidentally, due to how the SOM standard library is written, it also means 'self' is an SOMObject
    //
    Super
};

// Compile a 'self' call where 'self' is known to be an SOMObject
// 'isTopLevel == true' means this is a top-level statement and the return value may be safely discarded
//
void CompileSOMCall(TranslationContext& ctx, BlockTranslationContext& bctx, AstExpr* receiver, AstSymbol* selector, std::span<AstExpr*> args, uint32_t clobberSlot, uint32_t destSlot, bool isTopLevel = false)
{
    VM* vm = VM_GetActiveVMForCurrentThread();

    ctx.UpdateTopSlot(destSlot);
    ctx.UpdateTopSlot(static_cast<uint32_t>(clobberSlot + x_numSlotsForStackFrameHeader + args.size()));
    SOMReceiverKind rcvKind = SOMReceiverKind::Normal;
    if (ReceiverIsSelf(receiver) && ctx.m_isSelfObject)
    {
        rcvKind = SOMReceiverKind::ObjectSelf;
    }
    else if (ReceiverIsSuper(receiver))
    {
        rcvKind = SOMReceiverKind::Super;
    }

    size_t selectorStringId = selector->m_globalOrd;

    // Inlining control flow structures is technically unsound,
    // but allowed by Smalltalk standard ("restrictive selectors") and also done by SOM++/TruffleSOM etc.
    // Also, even though Smalltalk standard allows these unsound inlining,
    // I don't think SOM++ has did it exactly correctly (it's missing some checks).
    // But my goal here is to replicate SOM++ behavior so I'll just do whatever SOM++ did.
    //
    bool performUnsoundInlining = true;

    if (performUnsoundInlining && vm->IsSelectorInlinableControlFlow(selectorStringId) && rcvKind != SOMReceiverKind::Super)
    {
        size_t methId = selectorStringId;
        if (methId == vm->m_stringIdForIfTrue || methId == vm->m_stringIdForIfFalse ||
            methId == vm->m_stringIdForIfNil || methId == vm->m_stringIdForIfNotNil)
        {
            TestAssert(args.size() == 1);
            if (args[0]->GetKind() == AstExprKind::NestedBlock)
            {
                AstNestedBlock* block = assert_cast<AstNestedBlock*>(args[0]);
                if (block->m_params.size() == 0)
                {
                    uint32_t condSlot;
                    if (!DetectTrivialLocalVarUse(ctx, bctx, receiver, condSlot /*out*/))
                    {
                        condSlot = clobberSlot;
                        CompileExpression(ctx, bctx, receiver, clobberSlot, condSlot /*destSlot*/);
                    }

                    size_t branchBcLoc = ctx.m_builder.GetCurLength();
                    bool needExplicitElseBranch = false;
                    if (methId == vm->m_stringIdForIfFalse)
                    {
                        if (isTopLevel)
                        {
                            ctx.m_builder.CreateBranchIfTrue({
                                .cond = Local(condSlot)
                            });
                        }
                        else if (destSlot >= clobberSlot)
                        {
                            ctx.m_builder.CreateStoreNilAndBranchIfTrue({
                                .cond = Local(condSlot),
                                .output = Local(destSlot)
                            });
                        }
                        else
                        {
                            // It's not safe to early-clobber destSlot
                            //
                            ctx.m_builder.CreateBranchIfTrue({
                                .cond = Local(condSlot)
                            });
                            needExplicitElseBranch = true;
                        }
                    }
                    else if (methId == vm->m_stringIdForIfTrue)
                    {
                        if (isTopLevel)
                        {
                            ctx.m_builder.CreateBranchIfFalse({
                                .cond = Local(condSlot)
                            });
                        }
                        else if (destSlot >= clobberSlot)
                        {
                            ctx.m_builder.CreateStoreNilAndBranchIfFalse({
                                .cond = Local(condSlot),
                                .output = Local(destSlot)
                            });
                        }
                        else
                        {
                            // It's not safe to early-clobber destSlot
                            //
                            ctx.m_builder.CreateBranchIfFalse({
                                .cond = Local(condSlot)
                            });
                            needExplicitElseBranch = true;
                        }
                    }
                    else if (methId == vm->m_stringIdForIfNil)
                    {
                        if (condSlot == destSlot || isTopLevel)
                        {
                            ctx.m_builder.CreateBranchIfNotNil({
                                .cond = Local(condSlot)
                            });
                        }
                        else if (destSlot >= clobberSlot)
                        {
                            ctx.m_builder.CreateCopyAndBranchIfNotNil({
                                .cond = Local(condSlot),
                                .output = Local(destSlot)
                            });
                        }
                        else
                        {
                            // It's not safe to early-clobber destSlot
                            //
                            ctx.m_builder.CreateBranchIfNotNil({
                                .cond = Local(condSlot)
                            });
                            needExplicitElseBranch = true;
                        }
                    }
                    else
                    {
                        TestAssert(methId == vm->m_stringIdForIfNotNil);
                        if (condSlot == destSlot || isTopLevel)
                        {
                            ctx.m_builder.CreateBranchIfNil({
                                .cond = Local(condSlot)
                            });
                        }
                        else if (destSlot >= clobberSlot)
                        {
                            ctx.m_builder.CreateCopyAndBranchIfNil({
                                .cond = Local(condSlot),
                                .output = Local(destSlot)
                            });
                        }
                        else
                        {
                            // It's not safe to early-clobber destSlot
                            //
                            ctx.m_builder.CreateBranchIfNil({
                                .cond = Local(condSlot)
                            });
                            needExplicitElseBranch = true;
                        }
                    }

                    InlineBlock(ctx, bctx, block, clobberSlot, destSlot /*destSlot*/, isTopLevel /*mayDiscardReturnValue*/);

                    if (!needExplicitElseBranch)
                    {
                        if (unlikely(!ctx.m_builder.SetBranchTarget(branchBcLoc, ctx.m_builder.GetCurLength())))
                        {
                            fprintf(stderr, "[ERROR] Branch exceeded bytecode maximum branch distance, function is too long.\n");
                            abort();
                        }
                    }
                    else
                    {
                        size_t elseBrLoc = ctx.m_builder.GetCurLength();
                        ctx.m_builder.CreateBranch();
                        if (unlikely(!ctx.m_builder.SetBranchTarget(branchBcLoc, ctx.m_builder.GetCurLength())))
                        {
                            fprintf(stderr, "[ERROR] Branch exceeded bytecode maximum branch distance, function is too long.\n");
                            abort();
                        }

                        if (methId == vm->m_stringIdForIfFalse || methId == vm->m_stringIdForIfTrue)
                        {
                            ctx.m_builder.CreateMov({
                                .input = TValue::Create<tNil>(),
                                .output = Local(destSlot)
                            });
                        }
                        else
                        {
                            TestAssert(methId == vm->m_stringIdForIfNil || methId == vm->m_stringIdForIfNotNil);
                            ctx.m_builder.CreateMov({
                                .input = Local(condSlot),
                                .output = Local(destSlot)
                            });
                        }
                        if (unlikely(!ctx.m_builder.SetBranchTarget(elseBrLoc, ctx.m_builder.GetCurLength())))
                        {
                            fprintf(stderr, "[ERROR] Branch exceeded bytecode maximum branch distance, function is too long.\n");
                            abort();
                        }
                    }
                    return;
                }
            }
        }

        if (methId == vm->m_stringIdForIfTrueIfFalse || methId == vm->m_stringIdForIfFalseIfTrue ||
            methId == vm->m_stringIdForIfNilIfNotNil || methId == vm->m_stringIdForIfNotNilIfNil)
        {
            TestAssert(args.size() == 2);
            if (args[0]->GetKind() == AstExprKind::NestedBlock && args[1]->GetKind() == AstExprKind::NestedBlock)
            {
                AstNestedBlock* block1 = assert_cast<AstNestedBlock*>(args[0]);
                AstNestedBlock* block2 = assert_cast<AstNestedBlock*>(args[1]);
                if (block1->m_params.size() == 0 && block2->m_params.size() == 0)
                {
                    uint32_t condSlot;
                    if (!DetectTrivialLocalVarUse(ctx, bctx, receiver, condSlot /*out*/))
                    {
                        condSlot = clobberSlot;
                        CompileExpression(ctx, bctx, receiver, clobberSlot, condSlot /*destSlot*/);
                    }

                    size_t branchBcLoc = ctx.m_builder.GetCurLength();
                    if (methId == vm->m_stringIdForIfTrueIfFalse)
                    {
                        ctx.m_builder.CreateBranchIfFalse({
                            .cond = Local(condSlot)
                        });
                    }
                    else if (methId == vm->m_stringIdForIfFalseIfTrue)
                    {
                        ctx.m_builder.CreateBranchIfTrue({
                            .cond = Local(condSlot)
                        });
                    }
                    else if (methId == vm->m_stringIdForIfNilIfNotNil)
                    {
                        ctx.m_builder.CreateBranchIfNotNil({
                            .cond = Local(condSlot)
                        });
                    }
                    else
                    {
                        TestAssert(methId == vm->m_stringIdForIfNotNilIfNil);
                        ctx.m_builder.CreateBranchIfNil({
                            .cond = Local(condSlot)
                        });
                    }
                    InlineBlock(ctx, bctx, block1, clobberSlot, destSlot, isTopLevel /*mayDiscardReturnValue*/);
                    size_t elseBranchBcLoc = ctx.m_builder.GetCurLength();
                    ctx.m_builder.CreateBranch();
                    if (unlikely(!ctx.m_builder.SetBranchTarget(branchBcLoc, ctx.m_builder.GetCurLength())))
                    {
                        fprintf(stderr, "[ERROR] Branch exceeded bytecode maximum branch distance, function is too long.\n");
                        abort();
                    }
                    InlineBlock(ctx, bctx, block2, clobberSlot, destSlot, false /*mayDiscardReturnValue*/);
                    if (unlikely(!ctx.m_builder.SetBranchTarget(elseBranchBcLoc, ctx.m_builder.GetCurLength())))
                    {
                        fprintf(stderr, "[ERROR] Branch exceeded bytecode maximum branch distance, function is too long.\n");
                        abort();
                    }
                    return;
                }
            }
        }

        if (methId == vm->m_stringIdForMethodAnd || methId == vm->m_stringIdForMethodOr ||
            methId == vm->m_stringIdForOperatorAnd || methId == vm->m_stringIdForOperatorOr)
        {
            TestAssert(args.size() == 1);
            if (args[0]->GetKind() == AstExprKind::NestedBlock)
            {
                AstNestedBlock* block = assert_cast<AstNestedBlock*>(args[0]);
                if (block->m_params.size() == 0)
                {
                    uint32_t condSlot;
                    if (!DetectTrivialLocalVarUse(ctx, bctx, receiver, condSlot /*out*/))
                    {
                        condSlot = clobberSlot;
                        CompileExpression(ctx, bctx, receiver, clobberSlot, condSlot /*destSlot*/);
                    }

                    size_t branchBcLoc = ctx.m_builder.GetCurLength();

                    bool needExplicitElseBranch = false;
                    if (methId == vm->m_stringIdForMethodAnd || methId == vm->m_stringIdForOperatorAnd)
                    {
                        if (condSlot == destSlot || isTopLevel)
                        {
                            ctx.m_builder.CreateBranchIfFalse({
                                .cond = Local(condSlot)
                            });
                        }
                        else if (destSlot >= clobberSlot)
                        {
                            ctx.m_builder.CreateCopyAndBranchIfFalse({
                                .cond = Local(condSlot),
                                .output = Local(destSlot)
                            });
                        }
                        else
                        {
                            // It's not safe to early-clobber destSlot
                            //
                            ctx.m_builder.CreateBranchIfFalse({
                                .cond = Local(condSlot)
                            });
                            needExplicitElseBranch = true;
                        }
                    }
                    else
                    {
                        TestAssert(methId == vm->m_stringIdForMethodOr || methId == vm->m_stringIdForOperatorOr);
                        if (condSlot == destSlot || isTopLevel)
                        {
                            ctx.m_builder.CreateBranchIfTrue({
                                .cond = Local(condSlot)
                            });
                        }
                        else if (destSlot >= clobberSlot)
                        {
                            ctx.m_builder.CreateCopyAndBranchIfTrue({
                                .cond = Local(condSlot),
                                .output = Local(destSlot)
                            });
                        }
                        else
                        {
                            // It's not safe to early-clobber destSlot
                            //
                            ctx.m_builder.CreateBranchIfTrue({
                                .cond = Local(condSlot)
                            });
                            needExplicitElseBranch = true;
                        }
                    }

                    InlineBlock(ctx, bctx, block, clobberSlot, destSlot /*destSlot*/, isTopLevel /*mayDiscardReturnValue*/);

                    if (!needExplicitElseBranch)
                    {
                        if (unlikely(!ctx.m_builder.SetBranchTarget(branchBcLoc, ctx.m_builder.GetCurLength())))
                        {
                            fprintf(stderr, "[ERROR] Branch exceeded bytecode maximum branch distance, function is too long.\n");
                            abort();
                        }
                    }
                    else
                    {
                        size_t elseBrLoc = ctx.m_builder.GetCurLength();
                        ctx.m_builder.CreateBranch();
                        if (unlikely(!ctx.m_builder.SetBranchTarget(branchBcLoc, ctx.m_builder.GetCurLength())))
                        {
                            fprintf(stderr, "[ERROR] Branch exceeded bytecode maximum branch distance, function is too long.\n");
                            abort();
                        }
                        ctx.m_builder.CreateMov({
                            .input = Local(condSlot),
                            .output = Local(destSlot)
                        });
                        if (unlikely(!ctx.m_builder.SetBranchTarget(elseBrLoc, ctx.m_builder.GetCurLength())))
                        {
                            fprintf(stderr, "[ERROR] Branch exceeded bytecode maximum branch distance, function is too long.\n");
                            abort();
                        }
                    }
                    return;
                }
            }
        }

        if (methId == vm->m_stringIdForWhileTrue || methId == vm->m_stringIdForWhileFalse)
        {
            TestAssert(args.size() == 1);
            if (receiver->GetKind() == AstExprKind::NestedBlock && args[0]->GetKind() == AstExprKind::NestedBlock)
            {
                AstNestedBlock* condBlock = assert_cast<AstNestedBlock*>(receiver);
                AstNestedBlock* stmtBlock = assert_cast<AstNestedBlock*>(args[0]);
                if (condBlock->m_params.size() == 0 && stmtBlock->m_params.size() == 0)
                {
                    size_t loopBeginOffset = ctx.m_builder.GetCurLength();
                    InlineBlock(ctx, bctx, condBlock, clobberSlot, clobberSlot /*destSlot*/, false /*mayDiscardReturnValue*/);

                    size_t checkCondOffset = ctx.m_builder.GetCurLength();
                    if (methId == vm->m_stringIdForWhileTrue)
                    {
                        ctx.m_builder.CreateBranchIfFalse({
                            .cond = Local(clobberSlot)
                        });
                    }
                    else
                    {
                        TestAssert(methId == vm->m_stringIdForWhileFalse);
                        ctx.m_builder.CreateBranchIfTrue({
                            .cond = Local(clobberSlot)
                        });
                    }

                    InlineBlock(ctx, bctx, stmtBlock, clobberSlot, clobberSlot /*destSlot*/, true /*mayDiscardReturnValue*/);

                    size_t endBranchBcLoc = ctx.m_builder.GetCurLength();
                    ctx.m_builder.CreateBranchLoopHint();
                    if (unlikely(!ctx.m_builder.SetBranchTarget(endBranchBcLoc, loopBeginOffset)))
                    {
                        fprintf(stderr, "[ERROR] Branch exceeded bytecode maximum branch distance, function is too long.\n");
                        abort();
                    }
                    if (unlikely(!ctx.m_builder.SetBranchTarget(checkCondOffset, ctx.m_builder.GetCurLength())))
                    {
                        fprintf(stderr, "[ERROR] Branch exceeded bytecode maximum branch distance, function is too long.\n");
                        abort();
                    }
                    if (!isTopLevel)
                    {
                        ctx.m_builder.CreateMov({
                            .input = TValue::Create<tNil>(),
                            .output = Local(destSlot)
                        });
                    }
                    return;
                }
            }
        }

        if (methId == vm->m_stringIdForToDo || methId == vm->m_stringIdForDowntoDo)
        {
            TestAssert(args.size() == 2);
            if (args[1]->GetKind() == AstExprKind::NestedBlock)
            {
                AstNestedBlock* stmtBlock = assert_cast<AstNestedBlock*>(args[1]);

                if (stmtBlock->m_params.size() == 1)
                {
                    CompileExpression(ctx, bctx, receiver, clobberSlot, clobberSlot /*destSlot*/);

                    if (!isTopLevel)
                    {
                        ctx.m_builder.CreateMov({
                            .input = Local(clobberSlot),
                            .output = Local(clobberSlot + 1)
                        });
                        clobberSlot++;
                    }

                    CompileExpression(ctx, bctx, args[0], clobberSlot + 1, clobberSlot + 1 /*destSlot*/);

                    ctx.UpdateTopSlot(clobberSlot + 2);

                    size_t loopStartOffset = ctx.m_builder.GetCurLength();
                    if (methId == vm->m_stringIdForToDo)
                    {
                        ctx.m_builder.CreateCheckForLoopStartCond({
                            .val = Local(clobberSlot),
                            .limit = Local(clobberSlot + 1),
                            .output = Local(clobberSlot + 2)
                        });
                    }
                    else
                    {
                        TestAssert(methId == vm->m_stringIdForDowntoDo);
                        ctx.m_builder.CreateCheckDowntoForLoopStartCond({
                            .val = Local(clobberSlot),
                            .limit = Local(clobberSlot + 1),
                            .output = Local(clobberSlot + 2)
                        });
                    }

                    size_t bodyStartOffset = ctx.m_builder.GetCurLength();
                    InlineBlock(ctx, bctx, stmtBlock, clobberSlot + 2, clobberSlot + 2 /*destSlot*/, true /*mayDiscardReturnValue*/);

                    size_t loopCheckOffset = ctx.m_builder.GetCurLength();
                    if (methId == vm->m_stringIdForToDo)
                    {
                        ctx.m_builder.CreateForLoopStep({
                            .base = Local(clobberSlot)
                        });
                    }
                    else
                    {
                        TestAssert(methId == vm->m_stringIdForDowntoDo);
                        ctx.m_builder.CreateDowntoForLoopStep({
                            .base = Local(clobberSlot)
                        });
                    }

                    if (unlikely(!ctx.m_builder.SetBranchTarget(loopCheckOffset, bodyStartOffset)))
                    {
                        fprintf(stderr, "[ERROR] Branch exceeded bytecode maximum branch distance, function is too long.\n");
                        abort();
                    }
                    if (unlikely(!ctx.m_builder.SetBranchTarget(loopStartOffset, ctx.m_builder.GetCurLength())))
                    {
                        fprintf(stderr, "[ERROR] Branch exceeded bytecode maximum branch distance, function is too long.\n");
                        abort();
                    }
                    if (!isTopLevel && clobberSlot - 1 != destSlot)
                    {
                        ctx.m_builder.CreateMov({
                            .input = Local(clobberSlot - 1),
                            .output = Local(destSlot)
                        });
                    }
                    return;
                }
            }
        }
    }

    if (vm->IsSelectorArithmeticOperator(selectorStringId) && rcvKind != SOMReceiverKind::Super)
    {
        TestAssert(args.size() == 1);
        // If lhs or rhs is constant but not integer/double
        // skip since the fast path is known to not going to help
        //
        bool shouldSkip = false;
        if (selectorStringId != vm->m_strOperatorEqualEqual.m_id)
        {
            if (selectorStringId == vm->m_strOperatorAnd.m_id ||
                selectorStringId == vm->m_strOperatorLeftShift.m_id ||
                selectorStringId == vm->m_strOperatorRightShift.m_id ||
                selectorStringId == vm->m_strOperatorBitwiseXor.m_id)
            {
                if (receiver->IsLiteral() && receiver->GetKind() != AstExprKind::Integer)
                {
                    shouldSkip = true;
                }
                if (args[0]->IsLiteral() && args[0]->GetKind() != AstExprKind::Integer)
                {
                    shouldSkip = true;
                }
            }
            else if (selectorStringId == vm->m_strOperatorEqual.m_id)
            {
                if (receiver->IsLiteral() &&
                    receiver->GetKind() != AstExprKind::Integer && receiver->GetKind() != AstExprKind::Double &&
                    receiver->GetKind() != AstExprKind::String && receiver->GetKind() != AstExprKind::Symbol)
                {
                    shouldSkip = true;
                }
                if (args[0]->IsLiteral() &&
                    args[0]->GetKind() != AstExprKind::Integer && args[0]->GetKind() != AstExprKind::Double &&
                    args[0]->GetKind() != AstExprKind::String && args[0]->GetKind() != AstExprKind::Symbol)
                {
                    shouldSkip = true;
                }
            }
            else
            {
                if (receiver->IsLiteral() && receiver->GetKind() != AstExprKind::Integer && receiver->GetKind() != AstExprKind::Double)
                {
                    shouldSkip = true;
                }
                if (args[0]->IsLiteral() && args[0]->GetKind() != AstExprKind::Integer && args[0]->GetKind() != AstExprKind::Double)
                {
                    shouldSkip = true;
                }
            }
            if (!shouldSkip)
            {
                if (receiver->GetKind() == AstExprKind::VarUse &&
                    IsVariableResolvedToFalseTrueNil(ctx, bctx, assert_cast<AstVariableUse*>(receiver)->m_varInfo.m_name))
                {
                    shouldSkip = true;
                }
                if (args[0]->GetKind() == AstExprKind::VarUse &&
                    IsVariableResolvedToFalseTrueNil(ctx, bctx, assert_cast<AstVariableUse*>(args[0])->m_varInfo.m_name))
                {
                    shouldSkip = true;
                }
            }
        }

        if (!shouldSkip)
        {
            uint32_t curClobberSlot = clobberSlot;
            LocalOrCstWrapper lhs = Local(0);
            LocalOrCstWrapper rhs = Local(0);
            if (!DetectTrivialLocalVarOrConstantUse(ctx, bctx, receiver, lhs /*out*/))
            {
                lhs = Local(curClobberSlot);
                CompileExpression(ctx, bctx, receiver, curClobberSlot, curClobberSlot);
                curClobberSlot++;
            }
            if (!DetectTrivialLocalVarOrConstantUse(ctx, bctx, args[0], rhs /*out*/))
            {
                rhs = Local(curClobberSlot);
                CompileExpression(ctx, bctx, args[0], curClobberSlot, curClobberSlot);
                curClobberSlot++;
            }
            ctx.UpdateTopSlot(curClobberSlot);

            if (selectorStringId == vm->m_strOperatorPlus.m_id)
            {
                ctx.m_builder.CreateOperatorPlus({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorMinus.m_id)
            {
                ctx.m_builder.CreateOperatorMinus({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorStar.m_id)
            {
                ctx.m_builder.CreateOperatorStar({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorSlashSlash.m_id)
            {
                ctx.m_builder.CreateOperatorSlashSlash({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorPercent.m_id)
            {
                ctx.m_builder.CreateOperatorPercent({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorAnd.m_id)
            {
                ctx.m_builder.CreateOperatorAnd({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorEqual.m_id)
            {
                ctx.m_builder.CreateOperatorEqual({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorLessThan.m_id)
            {
                ctx.m_builder.CreateOperatorLessThan({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorLessEqual.m_id)
            {
                ctx.m_builder.CreateOperatorLessEqual({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorGreaterThan.m_id)
            {
                ctx.m_builder.CreateOperatorGreaterThan({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorGreaterEqual.m_id)
            {
                ctx.m_builder.CreateOperatorGreaterEqual({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorUnequal.m_id)
            {
                ctx.m_builder.CreateOperatorUnequal({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorTildeUnequal.m_id)
            {
                ctx.m_builder.CreateOperatorTildeUnequal({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorLeftShift.m_id)
            {
                ctx.m_builder.CreateOperatorLeftShift({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorRightShift.m_id)
            {
                ctx.m_builder.CreateOperatorRightShift({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorBitwiseXor.m_id)
            {
                ctx.m_builder.CreateOperatorBitwiseXor({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorSlash.m_id)
            {
                ctx.m_builder.CreateOperatorSlash({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            else
            {
                TestAssert(selectorStringId == vm->m_strOperatorEqualEqual.m_id);
                ctx.m_builder.CreateOperatorEqualEqual({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            return;
        }
    }

    if (args.size() == 1 && rcvKind != SOMReceiverKind::Super)
    {
        // Try the other binary operators
        // This must happen after trying control flow inlining,
        // since if the RHS of &&/|| is a lexical block we should always inline it instead of using this.
        //
        if (selectorStringId == vm->m_strOperatorLogicalAnd.m_id ||
            selectorStringId == vm->m_strOperatorKeywordAnd.m_id ||
            selectorStringId == vm->m_strOperatorLogicalOr.m_id ||
            selectorStringId == vm->m_strOperatorKeywordOr.m_id ||
            selectorStringId == vm->m_strOperatorValueColon.m_id ||
            selectorStringId == vm->m_strOperatorAtColon.m_id ||
            selectorStringId == vm->m_strOperatorCharAtColon.m_id)
        {
            uint32_t curClobberSlot = clobberSlot;
            Local lhs = Local(0);
            LocalOrCstWrapper rhs = Local(0);
            {
                uint32_t localVarSlot;
                if (DetectTrivialLocalVarUse(ctx, bctx, receiver, localVarSlot /*out*/))
                {
                    lhs = Local(localVarSlot);
                }
                else
                {
                    lhs = Local(curClobberSlot);
                    CompileExpression(ctx, bctx, receiver, curClobberSlot, curClobberSlot);
                    curClobberSlot++;
                }
            }
            // The bytecode supports constant RHS only if the operator is value: at: or char:
            //
            if (selectorStringId == vm->m_strOperatorValueColon.m_id ||
                selectorStringId == vm->m_strOperatorAtColon.m_id ||
                selectorStringId == vm->m_strOperatorCharAtColon.m_id)
            {
                if (!DetectTrivialLocalVarOrConstantUse(ctx, bctx, args[0], rhs /*out*/))
                {
                    rhs = Local(curClobberSlot);
                    CompileExpression(ctx, bctx, args[0], curClobberSlot, curClobberSlot);
                    curClobberSlot++;
                }
            }
            else
            {
                uint32_t localVarSlot;
                if (DetectTrivialLocalVarUse(ctx, bctx, args[0], localVarSlot /*out*/))
                {
                    rhs = Local(localVarSlot);
                }
                else
                {
                    rhs = Local(curClobberSlot);
                    CompileExpression(ctx, bctx, args[0], curClobberSlot, curClobberSlot);
                    curClobberSlot++;
                }
            }
            ctx.UpdateTopSlot(curClobberSlot);

            if (selectorStringId == vm->m_strOperatorLogicalAnd.m_id)
            {
                ctx.m_builder.CreateOperatorLogicalAnd({
                    .lhs = lhs,
                    .rhs = rhs.AsLocal(),
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorKeywordAnd.m_id)
            {
                ctx.m_builder.CreateOperatorKeywordAnd({
                    .lhs = lhs,
                    .rhs = rhs.AsLocal(),
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorLogicalOr.m_id)
            {
                ctx.m_builder.CreateOperatorLogicalOr({
                    .lhs = lhs,
                    .rhs = rhs.AsLocal(),
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorKeywordOr.m_id)
            {
                ctx.m_builder.CreateOperatorKeywordOr({
                    .lhs = lhs,
                    .rhs = rhs.AsLocal(),
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorValueColon.m_id)
            {
                ctx.m_builder.CreateOperatorValueColon({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorAtColon.m_id)
            {
                ctx.m_builder.CreateOperatorAtColon({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            else
            {
                TestAssert(selectorStringId == vm->m_strOperatorCharAtColon.m_id);
                ctx.m_builder.CreateOperatorCharAtColon({
                    .lhs = lhs,
                    .rhs = rhs,
                    .output = Local(destSlot)
                });
            }
            return;
        }
    }

    if (args.size() == 0 && rcvKind != SOMReceiverKind::Super)
    {
        if (vm->IsSelectorSpecializableUnaryOperator(selectorStringId))
        {
            uint32_t lhsSlot;
            if (!DetectTrivialLocalVarUse(ctx, bctx, receiver, lhsSlot /*out*/))
            {
                lhsSlot = clobberSlot;
                CompileExpression(ctx, bctx, receiver, clobberSlot, clobberSlot);
            }
            if (selectorStringId == vm->m_strOperatorAbs.m_id)
            {
                ctx.m_builder.CreateOperatorAbs({
                    .op = Local(lhsSlot),
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorSqrt.m_id)
            {
                ctx.m_builder.CreateOperatorSqrt({
                    .op = Local(lhsSlot),
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorIsNil.m_id)
            {
                ctx.m_builder.CreateOperatorIsNil({
                    .op = Local(lhsSlot),
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorNotNil.m_id)
            {
                ctx.m_builder.CreateOperatorNotNil({
                    .op = Local(lhsSlot),
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorValue.m_id)
            {
                ctx.m_builder.CreateOperatorValue({
                    .op = Local(lhsSlot),
                    .output = Local(destSlot)
                });
            }
            else if (selectorStringId == vm->m_strOperatorNot.m_id)
            {
                ctx.m_builder.CreateOperatorNot({
                    .op = Local(lhsSlot),
                    .output = Local(destSlot)
                });
            }
            else
            {
                TestAssert(selectorStringId == vm->m_strOperatorLength.m_id);
                ctx.m_builder.CreateOperatorLength({
                    .op = Local(lhsSlot),
                    .output = Local(destSlot)
                });
            }
            return;
        }
    }

    if (args.size() == 2 && rcvKind != SOMReceiverKind::Super)
    {
        if (selectorStringId == vm->m_strOperatorAtPut.m_id ||
            selectorStringId == vm->m_strOperatorValueWith.m_id)
        {
            uint32_t curClobberSlot = clobberSlot;
            uint32_t opSlot;
            if (!DetectTrivialLocalVarUse(ctx, bctx, receiver, opSlot /*out*/))
            {
                opSlot = curClobberSlot;
                CompileExpression(ctx, bctx, receiver, curClobberSlot, curClobberSlot);
                curClobberSlot++;
            }
            auto compileArg = [&](AstExpr* expr) ALWAYS_INLINE -> LocalOrCstWrapper
            {
                LocalOrCstWrapper res = Local(0);
                if (!DetectTrivialLocalVarOrConstantUse(ctx, bctx, expr, res /*out*/))
                {
                    res = Local(curClobberSlot);
                    CompileExpression(ctx, bctx, expr, curClobberSlot, curClobberSlot);
                    curClobberSlot++;
                }
                return res;
            };
            LocalOrCstWrapper arg1 = compileArg(args[0]);
            LocalOrCstWrapper arg2 = compileArg(args[1]);
            ctx.UpdateTopSlot(curClobberSlot);

            if (selectorStringId == vm->m_strOperatorAtPut.m_id)
            {
                ctx.m_builder.CreateOperatorAtPut({
                    .op = Local(opSlot),
                    .arg1 = arg1,
                    .arg2 = arg2,
                    .output = Local(destSlot)
                });
            }
            else
            {
                TestAssert(selectorStringId == vm->m_strOperatorValueWith.m_id);
                ctx.m_builder.CreateOperatorValueWith({
                    .op = Local(opSlot),
                    .arg1 = arg1,
                    .arg2 = arg2,
                    .output = Local(destSlot)
                });
            }
            return;
        }
    }

    uint32_t argBase = clobberSlot + x_numSlotsForStackFrameHeader;
    if (rcvKind == SOMReceiverKind::Normal)
    {
        CompileExpression(ctx, bctx, receiver, argBase /*clobberSlot*/, argBase /*destSlot*/);
    }

    uint32_t curArgDest = argBase + 1;
    for (AstExpr* arg : args)
    {
        CompileExpression(ctx, bctx, arg, curArgDest /*clobberSlot*/, curArgDest /*destSlot*/);
        curArgDest++;
    }

    SOMUniquedString meth {
        .m_id = static_cast<uint32_t>(selectorStringId),
        .m_hash = static_cast<uint32_t>(vm->m_interner.GetHash(selectorStringId))
    };
    TValue tv;
    tv.m_value = UnalignedLoad<uint64_t>(&meth);

    if (rcvKind == SOMReceiverKind::Normal)
    {
        ctx.m_builder.CreateSOMCall({
            .base = Local(clobberSlot),
            .meth = tv,
            .numArgs = SafeIntegerCast<uint16_t>(args.size() + 1),
            .output = Local(destSlot)
        });
    }
    else if (rcvKind == SOMReceiverKind::ObjectSelf)
    {
        ctx.m_builder.CreateSOMSelfCall({
            .base = Local(clobberSlot),
            .self = Local(0),
            .meth = tv,
            .numArgs = SafeIntegerCast<uint16_t>(args.size() + 1),
            .output = Local(destSlot)
        });
    }
    else
    {
        // TODO: this is nonsense. For a super call, we statically know the callee.
        // We should just pass the callee function, or if it is a trivial method, directly do the trivial method logic
        // (or if the method doesn't exist, trigger doesNotUnderstand)
        //
        TestAssert(rcvKind == SOMReceiverKind::Super);
        TestAssert(ctx.m_superClass != nullptr);
        TValue scTv; scTv.m_value = SystemHeapPointer<SOMClass>(ctx.m_superClass).m_value;
        ctx.m_builder.CreateSOMSuperCall({
            .base = Local(clobberSlot),
            .self = Local(0),
            .superClass = scTv,
            .meth = tv,
            .numArgs = SafeIntegerCast<uint16_t>(args.size() + 1),
            .output = Local(destSlot)
        });
    }
}

void CompileUnaryCall(TranslationContext& ctx, BlockTranslationContext& bctx, AstUnaryCall* node, uint32_t clobberSlot, uint32_t destSlot, bool isTopLevel = false)
{
    CompileSOMCall(ctx, bctx, node->m_receiver, node->m_selector, std::span<AstExpr*>{}, clobberSlot, destSlot, isTopLevel);
}

void CompileBinaryCall(TranslationContext& ctx, BlockTranslationContext& bctx, AstBinaryCall* node, uint32_t clobberSlot, uint32_t destSlot, bool isTopLevel = false)
{
    CompileSOMCall(ctx, bctx, node->m_receiver, node->m_selector, std::span<AstExpr*>{ &node->m_argument, 1 }, clobberSlot, destSlot, isTopLevel);
}

void CompileKeywordCall(TranslationContext& ctx, BlockTranslationContext& bctx, AstKeywordCall* node, uint32_t clobberSlot, uint32_t destSlot, bool isTopLevel = false)
{
    CompileSOMCall(ctx, bctx, node->m_receiver, node->m_selector, node->m_arguments, clobberSlot, destSlot, isTopLevel);
}

void CompileReturn(TranslationContext& ctx, BlockTranslationContext& bctx, AstExpr* node, uint32_t clobberSlot)
{
    CompileExpression(ctx, bctx, node, clobberSlot, clobberSlot /*destSlot*/);
    if (bctx.MustEmitUpvalueCloseBeforeReturn())
    {
        ctx.m_builder.CreateUpvalueClose({
            .base = Local(0)
        });
    }
    ctx.m_builder.CreateRet({
        .retStart = Local(clobberSlot),
        .numRet = 1
    });
}

void CompileThrow(TranslationContext& ctx, BlockTranslationContext& bctx, AstExpr* node, uint32_t clobberSlot)
{
    bctx.m_blockMayThrow = true;
    CompileExpression(ctx, bctx, node, clobberSlot, clobberSlot /*destSlot*/);
    ctx.m_builder.CreateSOMThrow({
        .value = Local(clobberSlot)
    });
}

void InstallLocalVariable(TranslationContext& ctx, BlockTranslationContext& bctx, std::string_view name, uint32_t slot)
{
    LocalVarInfo* vi = ctx.m_alloc.AllocateObject<LocalVarInfo>(&bctx, slot);
    auto rs = ctx.m_localVarMap.emplace(std::make_pair(name, TempVector<LocalVarInfo*>(ctx.m_alloc)));
    rs.first->second.push_back(vi);
}

// Returns the first free slot
//
uint32_t WARN_UNUSED InstallLocalVariables(TranslationContext& ctx, BlockTranslationContext& bctx, AstBlock* block, uint32_t startSlot)
{
    for (VariableInfo& vi : block->m_params)
    {
        InstallLocalVariable(ctx, bctx, vi.m_name, startSlot);
        startSlot++;
    }
    if (block->m_locals.size() > 0)
    {
        ctx.m_builder.CreateRangeFillNils({
            .base = Local(startSlot),
            .numToPut = SafeIntegerCast<uint16_t>(block->m_locals.size())
        });
    }
    for (VariableInfo& vi : block->m_locals)
    {
        InstallLocalVariable(ctx, bctx, vi.m_name, startSlot);
        startSlot++;
    }
    return startSlot;
}

void UninstallLocalVariable(TranslationContext& ctx, BlockTranslationContext& bctx, std::string_view name)
{
    auto it = ctx.m_localVarMap.find(name);
    TestAssert(it != ctx.m_localVarMap.end());
    TestAssert(!it->second.empty());
    TestAssert(it->second.back()->m_bctx == &bctx);
    std::ignore = bctx;
    it->second.pop_back();
}

void UninstallLocalVariables(TranslationContext& ctx, BlockTranslationContext& bctx, AstBlock* block)
{
    for (size_t i = block->m_locals.size(); i--;)
    {
        UninstallLocalVariable(ctx, bctx, block->m_locals[i].m_name);
    }
    for (size_t i = block->m_params.size(); i--;)
    {
        UninstallLocalVariable(ctx, bctx, block->m_params[i].m_name);
    }
}

void CompileBlockBody(TranslationContext& ctx, BlockTranslationContext& bctx, AstBlock* block, uint32_t clobberSlot)
{
    bool shouldReturnSelf = false;
    if (block->m_body.size() == 0)
    {
        if (!bctx.IsTopLevelMethod())
        {
            // For an empty block, the behavior is to return nil
            //
            if (bctx.IsInlined())
            {
                if (bctx.ReturnValueMayBeDiscarded())
                {
                    /* nothing to do */
                }
                else
                {
                    ctx.m_builder.CreateMov({
                        .input = TValue::Create<tNil>(),
                        .output = Local(bctx.GetReturnValueSlotForInlinedBlock())
                    });
                }
            }
            else
            {
                ctx.m_builder.CreateMov({
                    .input = TValue::Create<tNil>(),
                    .output = Local(0)
                });
                ctx.m_builder.CreateRet({
                    .retStart = Local(0),
                    .numRet = 1
                });
            }
        }
        else
        {
            // For an empty method, the behavior is to return self
            //
            ctx.m_builder.CreateRet({
                .retStart = Local(0),
                .numRet = 1
            });
        }
        return;
    }
    for (size_t i = 0; i < block->m_body.size(); i++)
    {
        AstExpr* expr = block->m_body[i];
        if (expr->GetKind() == AstExprKind::Return)
        {
            AstReturn* ret = assert_cast<AstReturn*>(expr);
            if (!bctx.IsTopLevelMethod())
            {
                // This is a block, the explicit return expression should be interpreted as a throw expression
                //
                if (bctx.ShouldCompileThrowIntoReturn())
                {
                    // Due to inlining, the throw expression should be compiled into a return expression
                    //
                    CompileReturn(ctx, bctx, ret->m_retVal, clobberSlot);
                }
                else
                {
                    CompileThrow(ctx, bctx, ret->m_retVal, clobberSlot);
                }
            }
            else
            {
                CompileReturn(ctx, bctx, ret->m_retVal, clobberSlot);
            }
            break;
        }
        if (i + 1 == block->m_body.size())
        {
            // Last statement is not a return, if it is a block, then it is implicitly a return
            //
            if (!bctx.IsTopLevelMethod())
            {
                // For block, the return value is the last statement's value
                //
                if (bctx.IsInlined())
                {
                    // Emit UpvalueClose if needed
                    //
                    if (bctx.MustEmitUpvalueCloseBeforeReturn())
                    {
                        ctx.m_builder.CreateUpvalueClose({
                            .base = Local(bctx.m_startSlot)
                        });
                    }
                    // If the block is inlined, the return value should be stored to bctx.ReturnValueSlot() instead
                    //
                    if (bctx.ReturnValueMayBeDiscarded() &&
                        (expr->IsLiteral() ||
                         (expr->GetKind() == AstExprKind::VarUse && !IsVariableResolvedToGlobal(ctx, bctx, assert_cast<AstVariableUse*>(expr)->m_varInfo.m_name)) ||
                         expr->GetKind() == AstExprKind::NestedBlock))
                    {
                        // The return value expression has no observable side effects (note that global get *may* have observable effect!),
                        // and the return value can be discarded, nothing to do
                        //
                    }
                    else
                    {
                        CompileExpression(ctx, bctx, expr, clobberSlot, bctx.GetReturnValueSlotForInlinedBlock());
                    }
                }
                else
                {
                    // The block is not inlined, should generate a normal return
                    //
                    CompileReturn(ctx, bctx, expr, clobberSlot);
                }
                break;
            }
            else
            {
                // This is a top-level method, we need to add a return self at the end after we generate bytecode for everything
                //
                shouldReturnSelf = true;
            }
        }
        // Compile the statement normally
        //
        if (expr->GetKind() == AstExprKind::Assignation)
        {
            CompileTopLevelAssignation(ctx, bctx, assert_cast<AstAssignation*>(expr), clobberSlot);
        }
        else if (expr->GetKind() == AstExprKind::UnaryCall)
        {
            CompileUnaryCall(ctx, bctx, assert_cast<AstUnaryCall*>(expr), clobberSlot, clobberSlot, true /*isTopLevel*/);
        }
        else if (expr->GetKind() == AstExprKind::BinaryCall)
        {
            CompileBinaryCall(ctx, bctx, assert_cast<AstBinaryCall*>(expr), clobberSlot, clobberSlot, true /*isTopLevel*/);
        }
        else if (expr->GetKind() == AstExprKind::KeywordCall)
        {
            CompileKeywordCall(ctx, bctx, assert_cast<AstKeywordCall*>(expr), clobberSlot, clobberSlot, true /*isTopLevel*/);
        }
        else
        {
            CompileExpression(ctx, bctx, expr, clobberSlot, clobberSlot /*destSlot*/);
        }
    }
    if (shouldReturnSelf)
    {
        TestAssert(bctx.IsTopLevelMethod());
        if (bctx.MustEmitUpvalueCloseBeforeReturn())
        {
            ctx.m_builder.CreateUpvalueClose({
                .base = Local(0)
            });
        }
        ctx.m_builder.CreateRet({
            .retStart = Local(0),
            .numRet = 1
        });
    }
}

// If 'block' has any arguments excluding self, the arguments excluding self must have been initialized at 'frameStartSlot'
//
void InlineBlock(TranslationContext& ctx,
                 BlockTranslationContext& bctx,
                 AstNestedBlock* block,
                 uint32_t frameStartSlot,
                 uint32_t destSlot,
                 bool mayDiscardReturnValue)
{
    ctx.UpdateTopSlot(destSlot);
    ctx.UpdateTopSlot(frameStartSlot);

    TestAssert(block->m_params.size() <= 2);

    BlockTranslationContext* newBCtx = ctx.m_alloc.AllocateObject<BlockTranslationContext>(ctx.m_alloc,
                                                                                           frameStartSlot /*startSlot*/,
                                                                                           &bctx /*lexicalParent*/,
                                                                                           bctx.m_owningContext /*owningContext*/,
                                                                                           bctx.m_trueParent /*trueParent*/);

    TestAssert(newBCtx->IsInlined());
    newBCtx->m_returnValueMayBeDiscarded = mayDiscardReturnValue;
    newBCtx->m_returnValueSlot = destSlot;

    uint32_t firstFreeSlot = InstallLocalVariables(ctx, *newBCtx, block, frameStartSlot /*startSlot*/);
    ctx.UpdateTopSlot(firstFreeSlot);

    CompileBlockBody(ctx, *newBCtx, block, firstFreeSlot);

    UninstallLocalVariables(ctx, *newBCtx, block);

    TestAssert(newBCtx->m_uvs.size() == 0 && newBCtx->m_uvOrdMap.size() == 0);

    if (newBCtx->m_blockMayThrow || newBCtx->m_containsThrowableBlocks)
    {
        bctx.m_containsThrowableBlocks = true;
    }
}

void CompileNewBlock(TranslationContext& ctx, BlockTranslationContext& bctx, AstNestedBlock* block, uint32_t /*clobberSlot*/, uint32_t destSlot)
{
    ctx.UpdateTopSlot(destSlot);

    TranslationContext* newCtx = ctx.m_alloc.AllocateObject<TranslationContext>(ctx.m_alloc,
                                                                                ctx.m_localVarMap,
                                                                                ctx.m_fieldMap,
                                                                                ctx.m_isSelfObject,
                                                                                ctx.m_superClass,
                                                                                ctx.m_results);

    newCtx->m_resultUcb->m_parent = ctx.m_resultUcb;
    newCtx->m_resultUcb->m_hasVariadicArguments = false;
    newCtx->m_resultUcb->m_numFixedArguments = static_cast<uint32_t>(block->m_params.size() + 1);

    TestAssert(block->m_params.size() <= 2);

    BlockTranslationContext* newBCtx = ctx.m_alloc.AllocateObject<BlockTranslationContext>(ctx.m_alloc,
                                                                                           &bctx /*lexicalParent*/,
                                                                                           bctx.m_owningContext /*trueParent*/);

    newBCtx->m_uvs.push_back({
        .m_isParentLocal = true,
        .m_parentLocalSlot = 0,
        .m_parentUpvalueOrd = static_cast<uint32_t>(-1),
        .m_varInfo = nullptr
    });

    newCtx->m_resultBCtx = newBCtx;

    uint32_t firstFreeSlot = InstallLocalVariables(*newCtx, *newBCtx, block, 1 /*startSlot*/);
    newCtx->m_historyTopSlot = firstFreeSlot;

    CompileBlockBody(*newCtx, *newBCtx, block, firstFreeSlot);

    UninstallLocalVariables(*newCtx, *newBCtx, block);

    if (newBCtx->m_blockMayThrow || newBCtx->m_containsThrowableBlocks)
    {
        bctx.m_containsThrowableBlocks = true;
    }

    bool selfMutable = newBCtx->MustMutablyCaptureSelf();
    if (block->m_params.size() == 0)
    {
        newCtx->m_resultUcb->m_fnKind = (selfMutable ? SOM_BlockNoArg : SOM_BlockNoArgImmSelf);
    }
    else if (block->m_params.size() == 1)
    {
        newCtx->m_resultUcb->m_fnKind = (selfMutable ? SOM_BlockOneArg : SOM_BlockOneArgImmSelf);
    }
    else
    {
        TestAssert(block->m_params.size() == 2);
        newCtx->m_resultUcb->m_fnKind = (selfMutable ? SOM_BlockTwoArgs : SOM_BlockTwoArgsImmSelf);
    }

    TValue tv; tv.m_value = reinterpret_cast<uint64_t>(newCtx->m_resultUcb);
    ctx.m_builder.CreateNewClosure({
        .unlinkedCb = tv,
        .output = Local(destSlot)
    });
}

void CompileExpression(TranslationContext& ctx, BlockTranslationContext& bctx, AstExpr* node, uint32_t clobberSlot, uint32_t destSlot)
{
    if (node->IsLiteral())
    {
        CompileAstLiteral(ctx, bctx, assert_cast<AstLiteral*>(node), clobberSlot, destSlot);
    }
    else if (node->GetKind() == AstExprKind::VarUse)
    {
        CompileVarUse(ctx, bctx, assert_cast<AstVariableUse*>(node), clobberSlot, destSlot);
    }
    else if (node->GetKind() == AstExprKind::Assignation)
    {
        CompileAssignation(ctx, bctx, assert_cast<AstAssignation*>(node), clobberSlot, destSlot);
    }
    else if (node->GetKind() == AstExprKind::NestedBlock)
    {
        CompileNewBlock(ctx, bctx, assert_cast<AstNestedBlock*>(node), clobberSlot, destSlot);
    }
    else
    {
        TestAssert(node->IsCall());
        if (node->GetKind() == AstExprKind::UnaryCall)
        {
            CompileUnaryCall(ctx, bctx, assert_cast<AstUnaryCall*>(node), clobberSlot, destSlot);
        }
        else if (node->GetKind() == AstExprKind::BinaryCall)
        {
            CompileBinaryCall(ctx, bctx, assert_cast<AstBinaryCall*>(node), clobberSlot, destSlot);
        }
        else
        {
            TestAssert(node->GetKind() == AstExprKind::KeywordCall);
            CompileKeywordCall(ctx, bctx, assert_cast<AstKeywordCall*>(node), clobberSlot, destSlot);
        }
    }
}

HeapPtr<FunctionObject> CompileMethod(TempArenaAllocator& alloc,
                                      SOMClass* superClass,
                                      TempUnorderedMap<std::string_view, uint32_t /*objectSlotOrd*/>& fieldMap,
                                      AstMethod* meth,
                                      bool isSelfObject,
                                      [[maybe_unused]] std::string_view className,
                                      [[maybe_unused]] bool isClassSide)
{
    VM* vm = VM_GetActiveVMForCurrentThread();
    TempUnorderedMap<std::string_view, TempVector<LocalVarInfo*>> localVarMap(alloc);

    TempVector<TranslationContext*> allCtx(alloc);

    TranslationContext* ctx = alloc.AllocateObject<TranslationContext>(alloc,
                                                                       localVarMap,
                                                                       fieldMap,
                                                                       isSelfObject,
                                                                       superClass,
                                                                       allCtx);

    BlockTranslationContext* bctx = alloc.AllocateObject<BlockTranslationContext>(alloc,
                                                                                  nullptr /*lexicalParent*/,
                                                                                  nullptr /*trueParent*/);
    ctx->m_resultBCtx = bctx;

#ifdef ENABLE_SOM_PROFILE_FREQUENCY
    {
        size_t fnProfileIdx = vm->GetMethodIndexForFrequencyProfiling(className, meth->m_selectorName->Get(&vm->m_interner), isClassSide);
        ctx->m_builder.CreateSOMProfileCallFreq({
            .idx = TValue::Create<tInt32>(SafeIntegerCast<int32_t>(fnProfileIdx))
        });
    }
#endif  // ifdef ENABLE_SOM_PROFILE_FREQUENCY

    uint32_t firstFreeSlot = InstallLocalVariables(*ctx, *bctx, meth, 1 /*startSlot*/);
    ctx->m_historyTopSlot = firstFreeSlot;

    // Determine if this method is a trivial method
    //
    {
        SOMMethodLookupResultKind triviality = SOM_NormalMethod;
        TValue literalRetVal;           // useful for LiteralReturn
        size_t globalOrFieldIdx = 0;        // useful for GlobalReturn/Getter/Setter
        if (meth->m_body.size() == 0)
        {
            // Empty method body, behavior is return self
            //
            triviality = SOM_SelfReturn;
        }
        else if (meth->m_body.size() == 1)
        {
            // If the method body consists of a single return statement,
            // the method may be a SelfReturn, LiteralReturn, GlobalReturn or Getter
            //
            if (meth->m_body[0]->GetKind() == AstExprKind::Return)
            {
                AstExpr* val = assert_cast<AstReturn*>(meth->m_body[0])->m_retVal;
                if (val->IsNonArrayLiteral())
                {
                    triviality = SOM_LiteralReturn;
                    literalRetVal = CreateConstantNonArray(assert_cast<AstLiteral*>(val));
                }
                else if (val->GetKind() == AstExprKind::VarUse)
                {
                    AstVariableUse* u = assert_cast<AstVariableUse*>(val);
                    VarResolveResult vr = ResolveVariable(*ctx, *bctx, u->m_varInfo.m_name, VarResolveMode::DryRun);
                    if (vr.m_kind == VarUseKind::Local)
                    {
                        if (u->m_varInfo.m_name == "self" || u->m_varInfo.m_name == "super")
                        {
                            triviality = SOM_SelfReturn;
                        }
                    }
                    else if (vr.m_kind == VarUseKind::Field)
                    {
                        triviality = SOM_Getter;
                        globalOrFieldIdx = vr.m_ord;
                    }
                    else if (vr.m_kind == VarUseKind::Global)
                    {
                        triviality = SOM_GlobalReturn;
                        globalOrFieldIdx = vr.m_ord;
                    }
                    else if (vr.m_kind == VarUseKind::FalseTrueNil)
                    {
                        triviality = SOM_LiteralReturn;
                        literalRetVal = vr.GetTValueForFalseTrueNil();
                    }
                    else
                    {
                        TestAssert(false && "unexpected");
                    }
                }
            }
        }

        if (meth->m_params.size() == 1)
        {
            // For one-arg method, check if it is a Setter
            // A setter should set a field to the argument value, and return self
            //
            AstAssignation* setterStmt = nullptr;
            if (meth->m_body.size() == 1)
            {
                if (meth->m_body[0]->GetKind() == AstExprKind::Assignation)
                {
                    setterStmt = assert_cast<AstAssignation*>(meth->m_body[0]);
                }
            }
            if (meth->m_body.size() == 2)
            {
                if (meth->m_body[0]->GetKind() == AstExprKind::Assignation)
                {
                    if (meth->m_body[1]->GetKind() == AstExprKind::Return)
                    {
                        AstReturn* retStmt = assert_cast<AstReturn*>(meth->m_body[1]);
                        if (retStmt->m_retVal->GetKind() == AstExprKind::VarUse)
                        {
                            std::string_view varName = assert_cast<AstVariableUse*>(retStmt->m_retVal)->m_varInfo.m_name;
                            if (varName == "self" || varName == "super")
                            {
                                setterStmt = assert_cast<AstAssignation*>(meth->m_body[0]);
                            }
                        }
                    }
                }
            }
            if (setterStmt != nullptr)
            {
                if (setterStmt->m_lhs.size() == 1)
                {
                    VarResolveResult vrLhs = ResolveVariable(*ctx, *bctx, setterStmt->m_lhs[0].m_name, VarResolveMode::DryRun);
                    if (vrLhs.m_kind == VarUseKind::Field)
                    {
                        if (setterStmt->m_rhs->GetKind() == AstExprKind::VarUse)
                        {
                            if (assert_cast<AstVariableUse*>(setterStmt->m_rhs)->m_varInfo.m_name == meth->m_params[0].m_name)
                            {
                                triviality = SOM_Setter;
                                globalOrFieldIdx = vrLhs.m_ord;
                            }
                        }
                    }
                }
            }
        }

        if (triviality != SOM_NormalMethod)
        {
            ctx->m_resultUcb->m_trivialFnType = triviality;
            if (triviality == SOM_LiteralReturn)
            {
                ctx->m_resultUcb->m_trivialFnInfo = literalRetVal.m_value;
            }
            else if (triviality != SOM_SelfReturn)
            {
                ctx->m_resultUcb->m_trivialFnInfo = globalOrFieldIdx;
            }
        }
    }

    CompileBlockBody(*ctx, *bctx, meth, firstFreeSlot);

    UninstallLocalVariables(*ctx, *bctx, meth);

#ifdef TESTBUILD
    for (auto& it : localVarMap)
    {
        TestAssert(it.second.empty());
    }
#endif

    TestAssert(bctx->m_uvs.size() == 0);

    for (TranslationContext* c : allCtx)
    {
        UnlinkedCodeBlock* ucb = c->m_resultUcb;
        BytecodeBuilder& bw = c->m_builder;

        ucb->m_stackFrameNumSlots = c->m_historyTopSlot + 1;
        ucb->m_numUpvalues = static_cast<uint32_t>(c->m_resultBCtx->m_uvs.size());
        ucb->m_upvalueInfo = new UpvalueMetadata[ucb->m_numUpvalues];
        for (size_t i = 0; i < ucb->m_numUpvalues; i++)
        {
            UpvalueMetadata& uv = ucb->m_upvalueInfo[i];
#ifndef NDEBUG
            uv.m_immutabilityFieldFinalized = true;
#endif
            if (i == 0)
            {
                // Note that despite that 'self' is never mutable, we must capture it as mutable if non-local return is possible,
                // since we need to use the open/closeness of this upvalue to determine if the
                // closure that created this closure is still on the stack frame to implement non-local return
                //
                TestAssert(ucb->m_fnKind == SOM_BlockNoArg || ucb->m_fnKind == SOM_BlockNoArgImmSelf ||
                           ucb->m_fnKind == SOM_BlockOneArg || ucb->m_fnKind == SOM_BlockOneArgImmSelf ||
                           ucb->m_fnKind == SOM_BlockTwoArgs || ucb->m_fnKind == SOM_BlockTwoArgsImmSelf);
                uv.m_isImmutable = (ucb->m_fnKind == SOM_BlockNoArgImmSelf ||
                                    ucb->m_fnKind == SOM_BlockOneArgImmSelf ||
                                    ucb->m_fnKind == SOM_BlockTwoArgsImmSelf);
                uv.m_isParentLocal = true;
                uv.m_slot = 0;
            }
            else
            {
                auto& info = c->m_resultBCtx->m_uvs[i];
                uv.m_isImmutable = info.m_varInfo->m_isImmutable;
                uv.m_isParentLocal = info.m_isParentLocal;
                uv.m_slot = info.m_isParentLocal ? info.m_parentLocalSlot : info.m_parentUpvalueOrd;
                TestAssert(uv.m_slot != static_cast<uint32_t>(-1));
            }
        }

        for (size_t offset : c->m_allUpvalueGetBytecodes)
        {
            Assert(bw.GetBytecodeKind(offset) == BCKind::UpvalueGetMutable);
            auto ops = bw.DecodeUpvalueGetMutable(offset);
            uint16_t ord = ops.ord.m_value;
            Assert(ord < ucb->m_numUpvalues);
            if (ucb->m_upvalueInfo[ord].m_isImmutable)
            {
                bw.ReplaceBytecode<BCKind::UpvalueGetImmutable>(offset, { .ord = ord, .output = ops.output });
            }
        }

        std::pair<uint8_t*, size_t> bytecodeData = bw.GetBuiltBytecodeSequence();
        std::pair<uint64_t*, size_t> constantTableData = bw.GetBuiltConstantTable();
        if (constantTableData.second >= 0x7fff)
        {
            // TODO: gracefully handle
            fprintf(stderr, "[LOCKDOWN] Bytecode contains too many constants. Limit 32766, got %llu.\n", static_cast<unsigned long long>(constantTableData.second));
            abort();
        }

        ucb->m_cstTableLength = static_cast<uint32_t>(constantTableData.second);
        ucb->m_cstTable = constantTableData.first;

        ucb->m_bytecode = bytecodeData.first;
        ucb->m_bytecodeLengthIncludingTailPadding = static_cast<uint32_t>(bytecodeData.second);
        ucb->m_bytecodeMetadataLength = bw.GetBytecodeMetadataTotalLength();
        const auto& bmUseCounts = bw.GetBytecodeMetadataUseCountArray();
        Assert(bmUseCounts.size() == x_num_bytecode_metadata_struct_kinds_);
        memcpy(ucb->m_bytecodeMetadataUseCounts, bmUseCounts.data(), bmUseCounts.size() * sizeof(uint16_t));

        bw.~BytecodeBuilder();
    }

#ifdef TESTBUILD
    for (TranslationContext* c : allCtx)
    {
        UnlinkedCodeBlock* ucb = c->m_resultUcb;
        for (size_t i = 0; i < ucb->m_numUpvalues; i++)
        {
            UpvalueMetadata& uv = ucb->m_upvalueInfo[i];
            TestAssert(ucb->m_parent != nullptr);
            if (uv.m_isParentLocal)
            {
                TestAssert(uv.m_slot < ucb->m_parent->m_stackFrameNumSlots);
            }
            else
            {
                TestAssert(uv.m_slot < ucb->m_parent->m_numUpvalues);
            }
        }
    }
#endif

    for (TranslationContext* c : allCtx)
    {
        UnlinkedCodeBlock* ucb = c->m_resultUcb;
        TestAssert(ucb->m_defaultCodeBlock == nullptr);
        ucb->m_defaultCodeBlock = CodeBlock::Create(vm, ucb, UserHeapPointer<void>{});
    }

    std::string debugName = std::string(className);
    if (isClassSide) { debugName += " class"; }
    debugName += ">>";
    debugName +=  meth->m_selectorName->Get(&vm->m_interner);
    ctx->m_resultUcb->m_debugName = std::move(debugName);

    CodeBlock* cb = ctx->m_resultUcb->m_defaultCodeBlock;
    TestAssert(cb->m_numUpvalues == 0);

    return FunctionObject::CreateAndFillUpvalues(cb,
                                                 nullptr /*CoroRuntimeCtx*/,
                                                 nullptr /*stackBase*/,
                                                 nullptr /*parent*/,
                                                 static_cast<size_t>(-1) /*selfOrdInStackFrame*/).As();
}

// Populate the SOMClass's method array and static method array after both the class and the class-class is available
//
void FinishSOMClassMethodArray(SOMClass* cl)
{
    SOMObject* classObj = cl->m_classObject;
    TestAssert(classObj != nullptr);
    TValue holder = TValue::Create<tObject>(TranslateToHeapPtr(classObj));
    SOMObject* meths = cl->m_methods;
    TestAssert(meths != nullptr);
    size_t len = meths->m_data[0].m_value;
    for (size_t i = 1; i <= len; i++)
    {
        TestAssert(meths->m_data[i].Is<tObject>());
        meths->m_data[i].As<tObject>()->m_data[0].m_value = holder.m_value;
    }
}

SOMClass* WARN_UNUSED SOMCompileFile(std::string className, bool isSystemClass)
{
    VM* vm = VM_GetActiveVMForCurrentThread();
    StringInterner* interner = &vm->m_interner;
    size_t stringId = interner->InternString(className);

    if (vm->m_parsedClasses.count(stringId))
    {
        return vm->m_parsedClasses[stringId];
    }
    Auto(TestAssert(vm->m_parsedClasses.count(stringId)));

    std::ifstream inFile = OpenFileForClass(className);
    TempArenaAllocator alloc;

    SOMParser parser(alloc, interner, className, inFile);

    AstClass* cl = parser.ParseClass();

    TestAssertIff(className == "Object", cl->m_superClass == nullptr);

    if (className != cl->m_name->Get(interner))
    {
        fprintf(stderr, "Class defined in %s.som is not %s.\n", className.c_str(), className.c_str());
        abort();
    }

    // System classes may have primitives that are defined as non-primitive in SOM code
    // or not defined in the SOM code at all, or in some cases, doesn't even exist in the base class!
    // (For example, SOM++ defined Double.min / Double.max which isn't defined anywhere in SOM standard library..)
    //
    // Forcefully change / add methods which we have primitive implementations
    //
    if (isSystemClass)
    {
        std::unordered_map<std::string_view, AstMethod*> methNameMap[2];
        for (AstMethod* meth : cl->m_classMethods)
        {
            std::string_view methName = meth->m_selectorName->Get(interner);
            TestAssert(!methNameMap[1].count(methName));
            methNameMap[1][methName] = meth;
        }
        for (AstMethod* meth : cl->m_instanceMethods)
        {
            std::string_view methName = meth->m_selectorName->Get(interner);
            TestAssert(!methNameMap[0].count(methName));
            methNameMap[0][methName] = meth;
        }

        auto createPrimitiveMethod = [&](std::string_view methName) ALWAYS_INLINE
        {
            AstSymbol* sym = alloc.AllocateObject<AstSymbol>();
            sym->m_globalOrd = interner->InternString(methName);
            AstMethod* meth = alloc.AllocateObject<AstMethod>(alloc);
            meth->m_isPrimitive = true;
            meth->m_selectorName = sym;
            return meth;
        };

        vm->m_somPrimitives.ForEachPrimitiveInClass(
            className,
            [&](std::string_view methName, bool isClassSide) ALWAYS_INLINE
            {
                auto it = methNameMap[static_cast<size_t>(isClassSide)].find(methName);
                if (it == methNameMap[static_cast<size_t>(isClassSide)].end())
                {
                    // Create a new AstMethod and insert it into the class
                    // Note that unlike a real AstMethod, we don't have the valid params array set up, but later logic doesn't care about it
                    //
                    AstMethod* meth = createPrimitiveMethod(methName);
                    if (isClassSide)
                    {
                        cl->m_classMethods.push_back(meth);
                    }
                    else
                    {
                        cl->m_instanceMethods.push_back(meth);
                    }
                }
                else
                {
                    // Forcefully change the method to primitive even if it wasn't in the SOM file
                    //
                    it->second->m_isPrimitive = true;
                }
            });
    }

    SOMClass* res = nullptr;
    // System classes have their hidden classes pre-allocated upfront to avoid chicken-and-egg problems
    //
    if (isSystemClass)
    {
        HeapPtr<SOMClass> preallocatedAddr = nullptr;
        if (className == "Object") { preallocatedAddr = vm->m_objectClass; }
        else if (className == "Class") { preallocatedAddr = vm->m_classClass; }
        else if (className == "Metaclass") { preallocatedAddr = vm->m_metaclassClass; }
        else if (className == "Array") { preallocatedAddr = vm->m_arrayHiddenClass; }
        else if (className == "String") { preallocatedAddr = vm->m_stringHiddenClass; }
        else if (className == "Nil") { preallocatedAddr = vm->m_nilClass; }
        else if (className == "Boolean") { preallocatedAddr = vm->m_booleanClass; }
        else if (className == "True") { preallocatedAddr = vm->m_trueClass; }
        else if (className == "False") { preallocatedAddr = vm->m_falseClass; }
        else if (className == "Integer") { preallocatedAddr = vm->m_integerClass; }
        else if (className == "Double") { preallocatedAddr = vm->m_doubleClass; }
        else if (className == "Block") { preallocatedAddr = vm->m_blockClass; }
        else if (className == "Block1") { preallocatedAddr = vm->m_block1Class; }
        else if (className == "Block2") { preallocatedAddr = vm->m_block2Class; }
        else if (className == "Block3") { preallocatedAddr = vm->m_block3Class; }
        else if (className == "Method") { preallocatedAddr = vm->m_methodClass; }
        else if (className == "Symbol") { preallocatedAddr = vm->m_symbolClass; }
        else if (className == "Primitive") { preallocatedAddr = vm->m_primitiveClass; }
        else if (className == "System") { preallocatedAddr = vm->m_systemClass; }
        else { ReleaseAssert(false && "unknown system class name!"); }
        TestAssert(preallocatedAddr != nullptr);
        res = TranslateToRawPointer(vm, preallocatedAddr);
    }

    {
        TempUnorderedMap<std::string_view, uint32_t /*objectSlotOrd*/> fieldMap(alloc);
        uint32_t curFieldOrd = 0;
        if (cl->m_superClass != nullptr)
        {
            for (size_t i = 0; i < cl->m_superClass->m_numFields; i++)
            {
                TValue tv = cl->m_superClass->m_fields->m_data[i + 1];
                TestAssert(tv.Is<tObject>());
                SOMObject* str = TranslateToRawPointer(vm, tv.As<tObject>());
                TestAssert(str->m_hiddenClass == SystemHeapPointer<SOMClass>(vm->m_symbolClass).m_value);
                std::string_view strVal(reinterpret_cast<char*>(&str->m_data[1]), str->m_data[0].m_value);
                TestAssert(!fieldMap.count(strVal));
                fieldMap[strVal] = curFieldOrd;
                curFieldOrd++;
            }
        }
        for (VariableInfo& vi : cl->m_instanceFields)
        {
            std::string_view varName = vi.m_name;
            TestAssert(!fieldMap.count(varName));
            fieldMap[varName] = curFieldOrd;
            curFieldOrd++;
        }

        bool isSelfObject = true;
        if (className == "Nil" || className == "False" || className == "True" || className == "Boolean" ||
            className == "Integer" || className == "Double" || className == "Block1" || className == "Block2" ||
            className == "Block3" || className == "Block" || className == "Method" || className == "Object")
        {
            isSelfObject = false;
        }

        for (AstMethod* meth : cl->m_instanceMethods)
        {
            HeapPtr<FunctionObject> fn;
            if (!meth->m_isPrimitive)
            {
                fn = CompileMethod(alloc, cl->m_superClass, fieldMap, meth, isSelfObject, className, false /*isClassSide*/);
            }
            else
            {
                fn = vm->m_somPrimitives.Get(className, meth->m_selectorName->Get(interner), false /*isClassSide*/);
            }
            meth->m_compilationResult = TranslateToRawPointer(vm, fn);
        }
    }

    {
        TempVector<uint32_t> fields(alloc);
        TempVector<SOMClass::MethInfo> methods(alloc);

        for (VariableInfo& vi : cl->m_instanceFields)
        {
            fields.push_back(static_cast<uint32_t>(interner->InternString(vi.m_name)));
        }
        for (AstMethod* meth : cl->m_instanceMethods)
        {
            methods.push_back({
                .m_stringId = static_cast<uint32_t>(meth->m_selectorName->m_globalOrd),
                .m_isPrimitive = meth->m_isPrimitive,
                .m_fnObj = meth->m_compilationResult
            });
        }
        // The "class" instance needs a hidden field since it may be used as a class object, which needs to identify the class it is for
        //
        if (className == "Class")
        {
            TestAssert(cl->m_superClass->m_numFields == 0);
            TestAssert(fields.size() == 0);
            fields.push_back(static_cast<uint32_t>(interner->InternString("(classOfClassObject)")));
        }
        res = SOMClass::Create(interner,
                               className,
                               cl->m_superClass,
                               methods,
                               fields,
                               res);
    }

    // The class objects for these "root" classes are specially set up by caller logic
    //
    if (className != "Object" && className != "Class" && className != "Metaclass")
    {
        SOMClass* SClass = TranslateToRawPointer(vm, SystemHeapPointer<SOMClass>(res->m_superClass->m_classObject->m_hiddenClass).As());

        {
            TempUnorderedMap<std::string_view, uint32_t /*objectSlotOrd*/> fieldMap(alloc);
            uint32_t curFieldOrd = 0;
            for (size_t i = 0; i < SClass->m_numFields; i++)
            {
                TValue tv = SClass->m_fields->m_data[i + 1];
                TestAssert(tv.Is<tObject>());
                SOMObject* str = TranslateToRawPointer(vm, tv.As<tObject>());
                TestAssert(str->m_hiddenClass == SystemHeapPointer<SOMClass>(vm->m_symbolClass).m_value);
                std::string_view strVal(reinterpret_cast<char*>(&str->m_data[1]), str->m_data[0].m_value);
                TestAssert(!fieldMap.count(strVal));
                fieldMap[strVal] = curFieldOrd;
                curFieldOrd++;
            }

            for (VariableInfo& vi : cl->m_classFields)
            {
                std::string_view varName = vi.m_name;
                TestAssert(!fieldMap.count(varName));
                fieldMap[varName] = curFieldOrd;
                curFieldOrd++;
            }

            for (AstMethod* meth : cl->m_classMethods)
            {
                HeapPtr<FunctionObject> fn;
                if (!meth->m_isPrimitive)
                {
                    fn = CompileMethod(alloc, SClass /*superClass*/, fieldMap, meth, true /*isSelfObject*/, className, true /*isClassSide*/);
                }
                else
                {
                    fn = vm->m_somPrimitives.Get(className, meth->m_selectorName->Get(interner), true /*isClassSide*/);
                }
                meth->m_compilationResult = TranslateToRawPointer(vm, fn);
            }
        }

        TempVector<uint32_t> fields(alloc);
        TempVector<SOMClass::MethInfo> methods(alloc);
        for (VariableInfo& vi : cl->m_classFields)
        {
            fields.push_back(static_cast<uint32_t>(interner->InternString(vi.m_name)));
        }
        for (AstMethod* meth : cl->m_classMethods)
        {
            methods.push_back({
                .m_stringId = static_cast<uint32_t>(meth->m_selectorName->m_globalOrd),
                .m_isPrimitive = meth->m_isPrimitive,
                .m_fnObj = meth->m_compilationResult
            });
        }

        TestAssert(res->m_superClass != nullptr);
        TestAssert(res->m_superClass->m_classObject != nullptr);
        TestAssert(res->m_superClass->m_classObject->m_hiddenClass != 0);

        SOMClass* CClass = SOMClass::Create(interner,
                                            className + " class",
                                            SClass,
                                            methods,
                                            fields);

        TestAssert(vm->m_metaclassClassLoaded);
        CClass->m_classObject = TranslateToRawPointer(vm->m_metaclassClass)->Instantiate();
        TestAssert(vm->m_metaclassClass->m_numFields == 1);
        CClass->m_classObject->m_data[0].m_value = reinterpret_cast<uint64_t>(CClass);

        res->m_classObject = CClass->Instantiate();
        TestAssert(CClass->m_numFields > 0);
        res->m_classObject->m_data[0].m_value = reinterpret_cast<uint64_t>(res);

        SetSOMGlobal(vm, className, TValue::Create<tObject>(TranslateToHeapPtr(res->m_classObject)));

        FinishSOMClassMethodArray(res);
        FinishSOMClassMethodArray(CClass);
    }
    else
    {
        TestAssert(cl->m_classFields.size() == 0);
        TestAssert(cl->m_classMethods.size() == 0);
    }

    vm->m_parsedClasses[stringId] = res;

    //fprintf(stderr, "%s: %d\n", className.c_str(), static_cast<int>(res->m_methodHtMask));
    return res;
}

// This function returns the class object for a system class
// The class object should have class H where H should inherit "classClass" and has no fields of its own
// (since this function is only used for system classes)
// The class object of H should have hidden class "metaClassClass"
//
void SetupSystemClassObject(SOMClass* cl, std::string_view name, SOMClass* classClass, SOMClass* metaClassClass)
{
    VM* vm = VM_GetActiveVMForCurrentThread();

    SOMClass* H = SOMClass::Create(&vm->m_interner,
                                   name,
                                   classClass /*superClass*/,
                                   {} /*methods*/,
                                   {} /*fields*/);

    H->m_classObject = metaClassClass->Instantiate();
    TestAssert(metaClassClass->m_numFields == 1);
    H->m_classObject->m_data[0].m_value = reinterpret_cast<uint64_t>(H);

    cl->m_classObject = H->Instantiate();
    TestAssert(H->m_numFields > 0);
    cl->m_classObject->m_data[0].m_value = reinterpret_cast<uint64_t>(cl);

    FinishSOMClassMethodArray(H);
    FinishSOMClassMethodArray(cl);
}

SOMInitializationResult SOMBootstrapClassHierarchy()
{
    VM* vm = VM_GetActiveVMForCurrentThread();

    TestAssert(!vm->m_metaclassClassLoaded);

    vm->m_objectClass = SOMClass::AllocateUninitializedSystemClass();
    vm->m_classClass = SOMClass::AllocateUninitializedSystemClass();
    vm->m_metaclassClass = SOMClass::AllocateUninitializedSystemClass();
    vm->m_arrayHiddenClass = SOMClass::AllocateUninitializedSystemClass();
    vm->m_stringHiddenClass =SOMClass::AllocateUninitializedSystemClass();
    vm->m_nilClass = SOMClass::AllocateUninitializedSystemClass();
    vm->m_booleanClass = SOMClass::AllocateUninitializedSystemClass();
    vm->m_trueClass = SOMClass::AllocateUninitializedSystemClass();
    vm->m_falseClass = SOMClass::AllocateUninitializedSystemClass();
    vm->m_integerClass = SOMClass::AllocateUninitializedSystemClass();
    vm->m_doubleClass = SOMClass::AllocateUninitializedSystemClass();
    vm->m_blockClass = SOMClass::AllocateUninitializedSystemClass();
    vm->m_block1Class = SOMClass::AllocateUninitializedSystemClass();
    vm->m_block2Class = SOMClass::AllocateUninitializedSystemClass();
    vm->m_block3Class = SOMClass::AllocateUninitializedSystemClass();
    vm->m_methodClass = SOMClass::AllocateUninitializedSystemClass();
    vm->m_symbolClass = SOMClass::AllocateUninitializedSystemClass();
    vm->m_primitiveClass = SOMClass::AllocateUninitializedSystemClass();
    vm->m_systemClass = SOMClass::AllocateUninitializedSystemClass();

    // objectClass->m_superClass = nullptr (initialized by SOMCompileFile)
    // objectClass->m_classObject is nullptr now, needs to be initialized later
    //
    SOMClass* objectClass = SOMCompileFile("Object", true);

    // classClass->m_superClass = objectClass (initialized by SOMCompileFile)
    // classClass->m_classObject is nullptr now, needs to be initialized later
    //
    SOMClass* classClass = SOMCompileFile("Class", true);

    // metaClassClass->m_superClass = classClass (initialized by SOMCompileFile)
    // metaClassClass->m_classObject is nullptr now, needs to be initialized later
    //
    SOMClass* metaClassClass = SOMCompileFile("Metaclass", true);

    SetupSystemClassObject(objectClass, "Object class", classClass, metaClassClass);
    SetupSystemClassObject(classClass, "Class class", classClass, metaClassClass);
    SetupSystemClassObject(metaClassClass, "Metaclass class", classClass, metaClassClass);

    SetSOMGlobal(vm, "Object", TValue::Create<tObject>(TranslateToHeapPtr(objectClass->m_classObject)));
    SetSOMGlobal(vm, "Class", TValue::Create<tObject>(TranslateToHeapPtr(classClass->m_classObject)));
    SetSOMGlobal(vm, "Metaclass", TValue::Create<tObject>(TranslateToHeapPtr(metaClassClass->m_classObject)));

    vm->m_metaclassClassLoaded = true;

    // Now Object, Class, MetaClass and their transitively associated class objects should all be set up
    // Parse the remaining system classes
    // Ugly: must load in the order of dependency, since the SOMCompileFile triggered by the parser never set systemClass = true
    //
    std::ignore = SOMCompileFile("Array", true);
    std::ignore = SOMCompileFile("String", true);
    std::ignore = SOMCompileFile("Boolean", true);
    std::ignore = SOMCompileFile("Nil", true);
    std::ignore = SOMCompileFile("True", true);
    std::ignore = SOMCompileFile("False", true);
    std::ignore = SOMCompileFile("Integer", true);
    std::ignore = SOMCompileFile("Double", true);
    std::ignore = SOMCompileFile("Block", true);
    std::ignore = SOMCompileFile("Block1", true);
    std::ignore = SOMCompileFile("Block2", true);
    std::ignore = SOMCompileFile("Block3", true);
    std::ignore = SOMCompileFile("Method", true);
    std::ignore = SOMCompileFile("Symbol", true);
    std::ignore = SOMCompileFile("Primitive", true);
    SOMClass* systemClass = SOMCompileFile("System", true);

    SetSOMGlobal(vm, "true", TValue::Create<tBool>(true));
    SetSOMGlobal(vm, "false", TValue::Create<tBool>(false));
    SetSOMGlobal(vm, "nil", TValue::Create<tNil>());

    SOMObject* systemInstance = systemClass->Instantiate();
    SetSOMGlobal(vm, "system", TValue::Create<tObject>(TranslateToHeapPtr(systemInstance)));

    return {
        .m_systemClass = systemClass,
        .m_systemInstance = systemInstance
    };
}
