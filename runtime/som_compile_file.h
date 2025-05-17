#pragma once

#include "common_utils.h"
#include "heap_ptr_utils.h"

class SOMObject;
class SOMClass;
class FunctionObject;

extern std::vector<std::string> g_classLoadPaths;

SOMClass* WARN_UNUSED SOMCompileFile(std::string className, bool isSystemClass = false);

HeapPtr<FunctionObject> SOMGetMethodFromClass(SOMClass* c, std::string_view meth);

struct SOMInitializationResult
{
    SOMClass* m_systemClass;
    SOMObject* m_systemInstance;
};

SOMInitializationResult SOMBootstrapClassHierarchy();
