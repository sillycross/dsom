#include "deegen_api.h"
#include "runtime_utils.h"
#include "som_class.h"
#include "som_utils.h"

static bool WARN_UNUSED ALWAYS_INLINE IsClosureMethod(HeapPtr<FunctionObject> func)
{
    SOMDetailEntityType fnTy = static_cast<SOMDetailEntityType>(func->m_invalidArrayType & 15);
    TestAssert(fnTy == SOM_BlockNoArg || fnTy == SOM_BlockNoArgImmSelf ||
               fnTy == SOM_BlockOneArg || fnTy == SOM_BlockOneArgImmSelf ||
               fnTy == SOM_BlockTwoArgs || fnTy == SOM_BlockTwoArgsImmSelf ||
               fnTy == SOM_Method);
    return fnTy == SOM_Method;
}

DEEGEN_DEFINE_LIB_FUNC_CONTINUATION(EscapedBlockReturnCont)
{
    // If the escapedBlock handler returns, the block should not throw,
    // but should return normally to its parent with the return value being the value returned by the handler
    //
    StackFrameHeader* hdr = GetStackFrameHeader();
    TValue* retStart = GetReturnValuesBegin();
    LongJump(hdr, retStart, 1 /*numReturnValues*/);
}

// This is a fake library function that is used internally in deegen to implement the ThrowError API
// This is never directly called from outside, and the name is hardcoded
//
DEEGEN_DEFINE_LIB_FUNC(DeegenInternal_ThrowTValueErrorImpl)
{
    // We repurpose 'numArgs' to be the TValue storing the exception object
    //
    TValue exnObject; exnObject.m_value = GetNumArgs();

    // In SOM, we use exception to implement non-local return.
    //
    StackFrameHeader* hdr = GetStackFrameHeader();

    // The block that triggered this non-local return
    //
    StackFrameHeader* curHdr = hdr;
    HeapPtr<FunctionObject> block = curHdr->m_func;
    TestAssert(!IsClosureMethod(block));

    while (true)
    {
        TestAssert(block->m_numUpvalues > 0);
        Upvalue* uv = reinterpret_cast<Upvalue*>(block->m_upvalues[0].m_value);
        if (unlikely(uv->m_isClosed))
        {
            // The closure is no longer on the stack, trigger escapedBlock.
            //
            goto escaped_block;
        }
        // The Upvalue points at 'self', which is always local 0
        //
        TValue* ptr = uv->m_ptr;
        curHdr = reinterpret_cast<StackFrameHeader*>(ptr) - 1;
        block = curHdr->m_func;

#ifdef TESTBUILD
        // For sanity, assert that everything till now is valid:
        //
        {
            // 'curHdr' should really point to a stack frame header
            //
            StackFrameHeader* h = hdr;
            while (true)
            {
                if (h == curHdr) { break; }
                h = reinterpret_cast<StackFrameHeader*>(h->m_caller);
                TestAssert(h != nullptr);
                h -= 1;
            }

            // The block should really be a block
            //
            TestAssert(block->m_type == HeapEntityType::Function);

            // The block should be an ancestor of the throwing block
            //
            UnlinkedCodeBlock* throwingUcb = static_cast<HeapPtr<CodeBlock>>(TCGet(hdr->m_func->m_executable).As())->m_owner;
            UnlinkedCodeBlock* curUcb = static_cast<HeapPtr<CodeBlock>>(TCGet(block->m_executable).As())->m_owner;
            while (true)
            {
                if (throwingUcb == curUcb) { break; }
                throwingUcb = throwingUcb->m_parent;
                TestAssert(throwingUcb != nullptr);
            }
        }
#endif

        if (IsClosureMethod(block))
        {
            // This is the *method* that eventually created the throwing block, we should exit from this method
            //
            break;
        }
        // Otherwise, this is a *block* that eventually created the throwing block,
        // we should walk up the stack further to find its creator, eventualy to the *method*
        //
    }

    // At this point, 'curHdr' holds the method that eventually created the throwing block,
    // and we should return to its caller
    //
    {
        // TODO: FIXME we need to properly update the interpreter tier-up counter for each stack frame that is in the interpreter tier
        //
        // Close all upvalues >= curHdr
        //
        CoroutineRuntimeContext* currentCoro = GetCurrentCoroutine();
        TValue* retFrameLocals = reinterpret_cast<TValue*>(curHdr + 1);
        currentCoro->CloseUpvalues(retFrameLocals);

        // Store the return value and return to the caller of curHdr
        //
        retFrameLocals[0] = exnObject;
        LongJump(curHdr, retFrameLocals /*retStart*/, 1 /*numReturnValues*/);
    }

escaped_block:
    // The method that eventually created the throwing block is no longer on the stack
    // We need to call escapedBlock
    //
    {
        VM* vm = VM_GetActiveVMForCurrentThread();
        TValue* callbase = GetStackBase();
        CoroutineRuntimeContext* currentCoro = GetCurrentCoroutine();
        currentCoro->CloseUpvalues(callbase);

        HeapPtr<FunctionObject> blockFn = hdr->m_func;
        TestAssert(blockFn->m_numUpvalues >= 1);
        TestAssert(static_cast<SOMDetailEntityType>(blockFn->m_invalidArrayType & 15) == SOM_BlockNoArg ||
                   static_cast<SOMDetailEntityType>(blockFn->m_invalidArrayType & 15) == SOM_BlockOneArg ||
                   static_cast<SOMDetailEntityType>(blockFn->m_invalidArrayType & 15) == SOM_BlockTwoArgs);

        TValue blockSelf = *reinterpret_cast<Upvalue*>(blockFn->m_upvalues[0].m_value)->m_ptr;
        TValue blockTv = TValue::Create<tFunction>(blockFn);

        HeapPtr<SOMClass> cl = GetSOMClassOfAny(blockSelf);
        GeneralHeapPointer<FunctionObject> handler = SOMClass::GetMethod(cl, vm->m_escapedBlockHandler);
        TestAssert(handler.m_value != 0);
        callbase[0].m_value = reinterpret_cast<uint64_t>(handler.As());
        callbase[x_numSlotsForStackFrameHeader] = blockSelf;
        callbase[x_numSlotsForStackFrameHeader + 1] = blockTv;
        MakeInPlaceCall(callbase + x_numSlotsForStackFrameHeader, 2 /*numArgs*/, DEEGEN_LIB_FUNC_RETURN_CONTINUATION(EscapedBlockReturnCont));
    }
}

// This is a fake library function that is used internally in deegen to implement the ThrowError API (C-string case)
// It is a simple wrapper of DeegenInternal_ThrowTValueErrorImpl
// This is never directly called from outside, and the name is hardcoded
//
DEEGEN_DEFINE_LIB_FUNC(DeegenInternal_ThrowCStringErrorImpl)
{
    // We repurpose 'numArgs' to be the TValue storing the C string
    //
    const char* errorMsg = reinterpret_cast<const char*>(GetNumArgs());
    TValue tv = TValue::Create<tObject>(TranslateToHeapPtr(SOMObject::AllocateString(errorMsg)));
    ThrowError(tv);
}

DEEGEN_END_LIB_FUNC_DEFINITIONS
