#pragma once

#include "common_utils.h"
#include "heap_ptr_utils.h"
#include "memory_ptr.h"

class FunctionObject;

struct SOMPrimitivesContainer
{
    SOMPrimitivesContainer();

    HeapPtr<FunctionObject> WARN_UNUSED Get(std::string_view className, std::string_view methName, bool isClassSide);

    void Add(std::string_view className, std::string_view methName, bool isClassSide, void* func);

    struct Element
    {
        Element() : m_implPtr(nullptr), m_fnObj(nullptr) { }
        void* m_implPtr;
        HeapPtr<FunctionObject> m_fnObj;

        void InitFnObj();
    };

    // func should have prototype std::string_view methName, bool isClassSide
    //
    template<typename Func>
    void ALWAYS_INLINE ForEachPrimitiveInClass(std::string_view className, const Func& func)
    {
        for (bool classSide : { false, true })
        {
            auto& data = m_map[static_cast<size_t>(classSide)][className];
            for (auto& it : data)
            {
                if (it.second.m_implPtr != nullptr)
                {
                    func(it.first, classSide);
                }
            }
        }
    }

    // Used by PrintStackTrace only, so very slow..
    //
    std::string WARN_UNUSED LookupFunctionObject(FunctionObject* func)
    {
        for (bool classSide : { false, true })
        {
            for (auto& data : m_map[static_cast<size_t>(classSide)])
            {
                std::string_view className = data.first;
                for (auto& it : data.second)
                {
                    std::string_view methName = it.first;
                    if (it.second.m_fnObj != nullptr && TranslateToRawPointer(it.second.m_fnObj) == func)
                    {
                        return std::string(className) + (classSide ? " class>>" : ">>") + std::string(methName);
                    }
                }
            }
        }
        return "(unknown primitive)";
    }

    std::unordered_map<std::string_view, std::unordered_map<std::string_view, Element>> m_map[2];
};
