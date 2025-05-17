#include "define_deegen_common_snippet.h"
#include "runtime_utils.h"

static HeapPtr<void> DeegenSnippet_GetGlobalObjectFromCodeBlock(CodeBlock* cb)
{
    return cb->m_globalObject.As();
}

DEFINE_DEEGEN_COMMON_SNIPPET("GetGlobalObjectFromCodeBlock", DeegenSnippet_GetGlobalObjectFromCodeBlock)

