#pragma once

#include "common_utils.h"
#include "temp_arena_allocator.h"
#include "string_interner.h"

enum class AstExprKind : uint8_t
{
    Array,
    String,
    Symbol,
    Integer,
    Double,
    VarUse,
    NestedBlock,
    Assignation,
    Return,
    UnaryCall,
    BinaryCall,
    KeywordCall
};

struct AstExpr
{
    MAKE_NONCOPYABLE(AstExpr);
    MAKE_NONMOVABLE(AstExpr);

    virtual ~AstExpr() = default;
    AstExpr(AstExprKind kind) : m_kind(kind) { }

    AstExprKind WARN_UNUSED GetKind() { return m_kind; }

    bool WARN_UNUSED IsNonArrayLiteral()
    {
        return m_kind == AstExprKind::String ||
            m_kind == AstExprKind::Symbol ||
            m_kind == AstExprKind::Integer ||
            m_kind == AstExprKind::Double;
    }

    bool WARN_UNUSED IsLiteral()
    {
        return m_kind == AstExprKind::Array || IsNonArrayLiteral();
    }

    bool WARN_UNUSED IsCall()
    {
        return m_kind == AstExprKind::UnaryCall || m_kind == AstExprKind::BinaryCall || m_kind == AstExprKind::KeywordCall;
    }

    bool WARN_UNUSED IsPrimary()
    {
        return IsLiteral() || m_kind == AstExprKind::VarUse || m_kind == AstExprKind::NestedBlock;
    }

    template<typename T>
    T* WARN_UNUSED As()
    {
        return assert_cast<T*>(this);
    }

private:
    AstExprKind m_kind;
};

struct AstPrimary : AstExpr
{
    AstPrimary(AstExprKind kind) : AstExpr(kind) { TestAssert(IsPrimary()); }
};

struct AstClass;

// Describes a declaration of a variable
//
struct AstVariableInstance
{
    std::string_view m_name;
};

struct VariableInfo
{
    VariableInfo() : m_var(nullptr) { }
    VariableInfo(std::string_view name) : m_var(nullptr), m_name(name) { }

    // nullptr before resolved
    //
    AstVariableInstance* m_var;
    std::string_view m_name;
};

struct AstAssignation : AstExpr
{
    AstAssignation(TempArenaAllocator& alloc) : AstExpr(AstExprKind::Assignation), m_lhs(alloc), m_rhs(nullptr) { }

    TempVector<VariableInfo> m_lhs;
    AstExpr* m_rhs;
};

struct AstBlock
{
    virtual ~AstBlock() = default;
    AstBlock(TempArenaAllocator& alloc) : m_params(alloc), m_locals(alloc), m_body(alloc) { }

    virtual bool IsNestedBlock() = 0;

    TempVector<VariableInfo> m_params;
    TempVector<VariableInfo> m_locals;
    TempVector<AstExpr*> m_body;
};

struct AstNestedBlock final : AstPrimary, AstBlock
{
    AstNestedBlock(TempArenaAllocator& alloc) : AstPrimary(AstExprKind::NestedBlock), AstBlock(alloc), m_parent(nullptr) { }

    virtual bool IsNestedBlock() override { return true; }

    AstBlock* m_parent;
};

struct AstLiteral : AstPrimary
{
protected:
    AstLiteral(AstExprKind kind) : AstPrimary(kind) { TestAssert(IsLiteral()); }
};

struct AstArray final : AstLiteral
{
    AstArray(TempArenaAllocator& alloc) : AstLiteral(AstExprKind::Array), m_elements(alloc) { }

    TempVector<AstLiteral*> m_elements;
};

struct AstString final : AstLiteral
{
    AstString() : AstLiteral(AstExprKind::String), m_globalOrd(static_cast<size_t>(-1)) { }

    std::string_view Get(StringInterner* interner)
    {
        TestAssert(m_globalOrd != static_cast<size_t>(-1));
        return interner->Get(m_globalOrd);
    }

    size_t m_globalOrd;
};

struct AstSymbol final : AstLiteral
{
    AstSymbol() : AstLiteral(AstExprKind::Symbol), m_globalOrd(static_cast<size_t>(-1)) { }

    std::string_view Get(StringInterner* interner)
    {
        TestAssert(m_globalOrd != static_cast<size_t>(-1));
        return interner->Get(m_globalOrd);
    }

    size_t m_globalOrd;
};

struct AstInteger final : AstLiteral
{
    AstInteger() : AstLiteral(AstExprKind::Integer), m_value(0) { }
    AstInteger(int32_t value) : AstInteger() { m_value = value; }

    int32_t m_value;
};

struct AstDouble final : AstLiteral
{
    AstDouble() : AstLiteral(AstExprKind::Double), m_value(0) { }
    AstDouble(double value) : AstDouble() { m_value = value; }

    double m_value;
};

// Get the value of a variable
//
struct AstVariableUse final : AstPrimary
{
    AstVariableUse() : AstPrimary(AstExprKind::VarUse) { }

    VariableInfo m_varInfo;
};

struct AstMethodCall : AstExpr
{
protected:
    AstMethodCall(AstExprKind kind) : AstExpr(kind) { TestAssert(IsCall()); }
};

struct AstUnaryCall final : AstMethodCall
{
    AstUnaryCall() : AstMethodCall(AstExprKind::UnaryCall), m_selector(nullptr), m_receiver(nullptr) { }

    AstSymbol* m_selector;
    AstExpr* m_receiver;
};

struct AstBinaryCall final : AstMethodCall
{
    AstBinaryCall() : AstMethodCall(AstExprKind::BinaryCall), m_selector(nullptr), m_receiver(nullptr), m_argument(nullptr) { }

    AstSymbol* m_selector;
    AstExpr* m_receiver;
    AstExpr* m_argument;
};

struct AstKeywordCall final : AstMethodCall
{
    AstKeywordCall(TempArenaAllocator& alloc)
        : AstMethodCall(AstExprKind::KeywordCall)
        , m_selector(nullptr)
        , m_receiver(nullptr)
        , m_arguments(alloc)
    { }

    AstSymbol* m_selector;
    AstExpr* m_receiver;
    TempVector<AstExpr*> m_arguments;
};

struct AstReturn final : AstExpr
{
    AstReturn() : AstExpr(AstExprKind::Return), m_retVal(nullptr) { }

    AstExpr* m_retVal;
};

enum class AstMethodKind : uint8_t
{
    Unary,
    Binary,
    Keyword,
    Unknown
};

class FunctionObject;

struct AstMethod final : AstBlock
{
    AstMethod(TempArenaAllocator& alloc)
        : AstBlock(alloc)
        , m_kind(AstMethodKind::Unknown)
        , m_isPrimitive(false)
        , m_selectorName(nullptr)
        , m_compilationResult(nullptr)
    { }

    virtual bool IsNestedBlock() override { return false; }

    AstMethodKind m_kind;
    bool m_isPrimitive;
    AstSymbol* m_selectorName;
    FunctionObject* m_compilationResult;
};

class SOMClass;

struct AstClass
{
    AstClass(TempArenaAllocator& alloc)
        : m_name(nullptr)
        , m_superClass(nullptr)
        , m_instanceFields(alloc)
        , m_classFields(alloc)
        , m_instanceMethods(alloc)
        , m_classMethods(alloc)
    { }

    AstSymbol* m_name;
    SOMClass* m_superClass;
    TempVector<VariableInfo> m_instanceFields;
    TempVector<VariableInfo> m_classFields;
    TempVector<AstMethod*> m_instanceMethods;
    TempVector<AstMethod*> m_classMethods;
};
