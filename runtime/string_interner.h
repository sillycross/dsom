#pragma once

#include "common.h"
#include "temp_arena_allocator.h"
#include "hash_functions.h"

struct StringInterner
{
    size_t WARN_UNUSED InternString(std::string_view str)
    {
        auto it = m_map.find(str);
        if (it == m_map.end())
        {
            size_t count = m_map.size();
            char* s = m_alloc.AllocateArray<char>(str.size() + 1);
            memcpy(s, str.data(), str.size());
            s[str.size()] = '\0';
            std::string_view key(s, str.size());
            uint64_t hash = HashString(key.data(), key.size());
            m_map[key] = count;
            TestAssert(m_map.size() == count + 1);
            TestAssert(m_list.size() == count);
            m_list.push_back(std::make_pair(key, hash));
            return count;
        }
        else
        {
            return it->second;
        }
    }

    std::string_view Get(size_t ord)
    {
        TestAssert(ord < m_list.size());
        return m_list[ord].first;
    }

    uint64_t GetHash(size_t ord)
    {
        TestAssert(ord < m_list.size());
        return m_list[ord].second;
    }

    TempArenaAllocator m_alloc;
    std::unordered_map<std::string_view, size_t> m_map;
    std::vector<std::pair<std::string_view, uint64_t /*hash*/>> m_list;
};
