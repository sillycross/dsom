// This file is mostly adapted from SOM++'s parser (Parser.h and Parser.cpp)
// I can see many bugs on gracefully handling invalid programs. I'm not here to fix them so they are left as-is.
//
#pragma once

#include "common.h"
#include "som_lexer.h"
#include "som_ast.h"
#include "string_interner.h"
#include "som_compile_file.h"

struct SOMParser
{
    SOMParser(TempArenaAllocator& alloc,
              StringInterner* interner,
              const std::string& fileName,
              std::istream& fileStream)
        : m_alloc(alloc)
        , m_symbolInterner(interner)
        , m_fileName(fileName)
        , m_lexer(fileStream)
    {
        Eat();
    }

    static constexpr Symbol singleOpSyms[] = {Not,   And,  Or,    Star,  Div,
                                              Mod,   Plus, Equal, More,  Less,
                                              Comma, At,   Per,   Minus, NONE};

    static constexpr Symbol binaryOpSyms[] = {Or,   Comma, Minus, Equal, Not,  And,
                                              Or,   Star,  Div,   Mod,   Plus, Equal,
                                              More, Less,  Comma, At,    Per,  NONE};

    static constexpr Symbol keywordSelectorSyms[] = {Keyword, KeywordSequence, NONE};
    static constexpr Symbol identOrPrimitiveSyms[] = {Identifier, Primitive, NONE};

    void NO_RETURN ParseError(const char* msg, const std::string& expected)
    {
        std::string msgWithMeta =
            "%(file)s:%(line)d:%(column)d: error: " + std::string(msg);

        std::string found;
        if (m_curSym == Integer || m_curSym >= STString) {
            found = symnames[m_curSym] + std::string(" (") + m_curText + ")";
        } else {
            found = symnames[m_curSym];
        }

        auto ReplacePattern = [&](std::string& str, const char* pattern, const std::string& replacement) ALWAYS_INLINE
        {
            size_t const pos = str.find(pattern);
            if (pos == std::string::npos) { return; }
            str.replace(pos, strlen(pattern), replacement);
        };

        ReplacePattern(msgWithMeta, "%(file)s", m_fileName);

        std::string line = std::to_string(m_lexer.GetCurrentLineNumber());
        ReplacePattern(msgWithMeta, "%(line)d", line);

        std::string column = std::to_string(m_lexer.GetCurrentColumn());
        ReplacePattern(msgWithMeta, "%(column)d", column);
        ReplacePattern(msgWithMeta, "%(expected)s", expected);
        ReplacePattern(msgWithMeta, "%(found)s", found);

        fprintf(stderr, "%s\n", msgWithMeta.c_str());
        abort();
    }

    void NO_RETURN ParseError(const char* msg, Symbol expected)
    {
        std::string const expectedStr(symnames[expected]);
        ParseError(msg, expectedStr);
    }

    // Proceed to next symbol
    //
    void Eat()
    {
        m_curSym = m_lexer.GetSym();
        m_curText = m_lexer.GetText();
    }

    Symbol WARN_UNUSED Peek()
    {
        if (!m_lexer.GetPeekDone())
        {
            m_nextSym = m_lexer.Peek();
        }
        return m_nextSym;
    }

    // Return true and advance to next symbol if the current symbol is 'sym'
    //
    bool WARN_UNUSED Accept(Symbol sym)
    {
        if (m_curSym == sym) { Eat(); return true; }
        return false;
    }

    // 'ss' is a NONE-terminated array
    //
    bool WARN_UNUSED IsOneOf(const Symbol* ss)
    {
        while (*ss != NONE)
        {
            if (*ss++ == m_curSym)
            {
                return true;
            }
        }
        return false;
    }

    bool WARN_UNUSED AcceptOneOf(const Symbol* ss)
    {
        if (IsOneOf(ss))
        {
            Eat();
            return true;
        }
        return false;
    }

    // Check that the current symbol is 'sym', throw parse error if not. Advance to next symbol
    //
    void Expect(Symbol sym)
    {
        if (Accept(sym)) { return; }
        ParseError("Unexpected symbol. Expected %(expected)s, but found %(found)s\n", sym);
    }

    void ExpectOneOf(const Symbol* ss)
    {
        if (AcceptOneOf(ss)) { return; }
        bool first = true;
        std::string expectedStr;
        const Symbol* next = ss;
        while (*next != NONE)
        {
            if (!first) { expectedStr += ", "; }
            first = false;
            expectedStr += symnames[*next];
            next += 1;
        }
        ParseError(
            "Unexpected symbol. Expected one of %(expected)s, but found "
            "%(found)s\n",
            expectedStr);
    }

    AstInteger* WARN_UNUSED ParseInteger(const std::string& str, int base, bool shouldNegate)
    {
        errno = 0;

        char* pEnd = nullptr;

        int64_t i = std::strtoll(str.c_str(), &pEnd, base);

        if (str.c_str() + str.length() != pEnd) {
            // did not parse anything
            ReleaseAssert(false && "invalid integer representation in source code: should not reach here");
        }

        const bool rangeError = errno == ERANGE;
        if (rangeError) {
            // TODO(smarr): try a big int library
            fprintf(stderr, "WARNING: integer literal '%s' in source code that does not fit int64_t, treated as 0!\n",
                    str.c_str());
            i = 0;
        }

        if (shouldNegate) {
            i = -i;
        }

        if (!(std::numeric_limits<int32_t>::min() <= i && i <= std::numeric_limits<int32_t>::max()))
        {
            fprintf(stderr, "WARNING: integer literal in source code that does not fit int32_t, truncated from %lld to %d.\n",
                    static_cast<long long>(i), static_cast<int>(static_cast<int32_t>(i)));
        }

        return m_alloc.AllocateObject<AstInteger>(static_cast<int32_t>(i));
    }

    AstInteger* WARN_UNUSED ParseInteger(bool shouldNegate)
    {
        AstInteger* res = ParseInteger(m_curText, 10 /*base*/, shouldNegate);
        Expect(Integer);
        return res;
    }

    AstDouble* WARN_UNUSED ParseDouble(bool shouldNegate)
    {
        char* pEnd = nullptr;
        double d = std::strtod(m_curText.c_str(), &pEnd);
        ReleaseAssert(pEnd == m_curText.data() + m_curText.length());
        if (shouldNegate) { d = 0 - d; }
        Expect(Double);
        return m_alloc.AllocateObject<AstDouble>(d);
    }

    AstLiteral* WARN_UNUSED ParseNumber(bool shouldNegate)
    {
        if (m_curSym == Integer)
        {
            return ParseInteger(shouldNegate);
        }
        else
        {
            return ParseDouble(shouldNegate);
        }
    }

    AstLiteral* WARN_UNUSED ParseNumber()
    {
        if (m_curSym == Minus)
        {
            Eat();
            return ParseNumber(true /*shouldNegate*/);
        }
        else
        {
            return ParseNumber(false /*shouldNegate*/);
        }
    }

    AstString* WARN_UNUSED ParseString()
    {
        AstString* res = m_alloc.AllocateObject<AstString>();
        res->m_globalOrd = m_symbolInterner->InternString(m_curText);
        Expect(STString);
        return res;
    }

    AstSymbol* WARN_UNUSED ParseUnarySelector()
    {
        AstSymbol* res = m_alloc.AllocateObject<AstSymbol>();
        res->m_globalOrd = m_symbolInterner->InternString(m_curText);
        ExpectOneOf(identOrPrimitiveSyms);
        return res;
    }

    AstSymbol* WARN_UNUSED ParseBinarySelector()
    {
        AstSymbol* res = m_alloc.AllocateObject<AstSymbol>();
        res->m_globalOrd = m_symbolInterner->InternString(m_curText);
        if (AcceptOneOf(singleOpSyms) || Accept(OperatorSequence)) {
        } else {
            Expect(NONE);
        }
        return res;
    }

    AstSymbol* WARN_UNUSED ParseKeywordSelector()
    {
        AstSymbol* res = m_alloc.AllocateObject<AstSymbol>();
        res->m_globalOrd = m_symbolInterner->InternString(m_curText);
        ExpectOneOf(keywordSelectorSyms);
        return res;
    }

    AstSymbol* WARN_UNUSED ParseSelector()
    {
        if (m_curSym == OperatorSequence || IsOneOf(singleOpSyms))
        {
            return ParseBinarySelector();
        }
        if (IsOneOf(keywordSelectorSyms))
        {
            return ParseKeywordSelector();
        }
        return ParseUnarySelector();
    }

    AstSymbol* WARN_UNUSED ParseSymbol()
    {
        Expect(Pound);
        if (m_curSym == STString)
        {
            AstSymbol* res = m_alloc.AllocateObject<AstSymbol>();
            res->m_globalOrd = m_symbolInterner->InternString(m_curText);
            Expect(STString);
            return res;
        }
        return ParseSelector();
    }

    AstArray* WARN_UNUSED ParseArray()
    {
        AstArray* res = m_alloc.AllocateObject<AstArray>(m_alloc);
        Expect(Pound);
        Expect(NewTerm);
        while (m_curSym != EndTerm)
        {
            res->m_elements.push_back(ParseLiteral());
        }
        Expect(EndTerm);
        return res;
    }

    AstLiteral* WARN_UNUSED ParseLiteral()
    {
        if (m_curSym == Pound)
        {
            if (Peek() == NewTerm)
            {
                return ParseArray();
            }
            else
            {
                return ParseSymbol();
            }
        }
        if (m_curSym == STString)
        {
            return ParseString();
        }
        return ParseNumber();
    }

    std::string_view WARN_UNUSED ParseVariable()
    {
        char* s = m_alloc.AllocateArray<char>(m_curText.size() + 1);
        memcpy(s, m_curText.data(), m_curText.size());
        s[m_curText.size()] = '\0';
        std::string_view res(s, m_curText.size());
        ExpectOneOf(identOrPrimitiveSyms);
        return res;
    }

    AstExpr* WARN_UNUSED ParsePrimary()
    {
        if (IsOneOf(identOrPrimitiveSyms))
        {
            AstVariableUse* res = m_alloc.AllocateObject<AstVariableUse>();
            res->m_varInfo.m_name = ParseVariable();
            return res;
        }
        if (m_curSym == NewTerm)
        {
            return ParseNestedTerm();
        }
        if (m_curSym == NewBlock)
        {
            return ParseNestedBlock();
        }
        return ParseLiteral();
    }

    AstExpr* WARN_UNUSED ParseNestedTerm()
    {
        Expect(NewTerm);
        AstExpr* res = ParseExpr();
        Expect(EndTerm);
        return res;
    }

    void ParseParameters(TempVector<VariableInfo>& res /*out*/)
    {
        std::unordered_set<std::string_view> args;
        do {
            Expect(Colon);
            std::string_view argName = ParseVariable();
            if (args.count(argName))
            {
                ParseError("Duplicate arguments!", "");
            }
            args.insert(argName);
            AstVariableInstance* vdef = m_alloc.AllocateObject<AstVariableInstance>();
            vdef->m_name = argName;
            res.push_back(VariableInfo());
            VariableInfo& vi = res.back();
            vi.m_var = vdef;
            vi.m_name = argName;
        } while (m_curSym == Colon);
    }

    void ParseLocals(TempVector<VariableInfo>& res /*out*/)
    {
        if (Accept(Or))
        {
            std::unordered_set<std::string_view> args;
            while (IsOneOf(identOrPrimitiveSyms))
            {
                std::string_view argName = ParseVariable();
                if (args.count(argName))
                {
                    ParseError("Duplicate locals!", "");
                }
                args.insert(argName);
                AstVariableInstance* vdef = m_alloc.AllocateObject<AstVariableInstance>();
                vdef->m_name = argName;
                res.push_back(VariableInfo());
                VariableInfo& vi = res.back();
                vi.m_var = vdef;
                vi.m_name = argName;
                // fprintf(stderr, "Local %s\n", std::string(argName).c_str());
            }
            Expect(Or);
        }
    }

    void ParseBlockExprs(TempVector<AstExpr*>& res /*out*/)
    {
        while (true)
        {
            if (m_curSym == EndTerm || m_curSym == EndBlock)
            {
                return;
            }
            if (Accept(Exit))
            {
                AstExpr* retVal = ParseExpr();
                AstReturn* retExpr = m_alloc.AllocateObject<AstReturn>();
                retExpr->m_retVal = retVal;
                res.push_back(retExpr);
                std::ignore = Accept(Period);
                return;
            }
            res.push_back(ParseExpr());
            if (!Accept(Period))
            {
                return;
            }
        }
    }

    AstNestedBlock* WARN_UNUSED ParseNestedBlock()
    {
        AstNestedBlock* res = m_alloc.AllocateObject<AstNestedBlock>(m_alloc);
        Expect(NewBlock);
        if (m_curSym == Colon)
        {
            ParseParameters(res->m_params /*out*/);
            Expect(Or);
        }
        res->m_parent = m_currentBlock;
        m_currentBlock = res;
        ParseLocals(res->m_locals /*out*/);
        ParseBlockExprs(res->m_body /*out*/);
        m_currentBlock = res->m_parent;
        Expect(EndBlock);
        return res;
    }

    AstExpr* WARN_UNUSED ParseExpr()
    {
        if (Peek() == Assign)
        {
            return ParseAssignation();
        }
        else
        {
            return ParseEvaluation();
        }
    }

    std::string_view WARN_UNUSED ParseOneAssignment()
    {
        std::string_view res = ParseVariable();
        Expect(Assign);
        return res;
    }

    void ParseAssignments(TempVector<VariableInfo>& l /*inout*/)
    {
        if (IsOneOf(identOrPrimitiveSyms))
        {
            l.push_back(VariableInfo(ParseOneAssignment()));
            if (Peek() == Assign)
            {
                ParseAssignments(l);
            }
        }
    }

    AstAssignation* WARN_UNUSED ParseAssignation()
    {
        AstAssignation* res = m_alloc.AllocateObject<AstAssignation>(m_alloc);
        ParseAssignments(res->m_lhs /*inout*/);
        res->m_rhs = ParseEvaluation();
        return res;
    }

    AstUnaryCall* WARN_UNUSED ParseUnaryMessage(AstExpr* primary)
    {
        AstSymbol* method = ParseUnarySelector();
        AstUnaryCall* res = m_alloc.AllocateObject<AstUnaryCall>();
        res->m_selector = method;
        res->m_receiver = primary;
        return res;
    }

    AstExpr* WARN_UNUSED ParseBinaryOperand()
    {
        AstExpr* op = ParsePrimary();
        while (IsOneOf(identOrPrimitiveSyms))
        {
            op = ParseUnaryMessage(op);
        }
        return op;
    }

    AstBinaryCall* WARN_UNUSED ParseBinaryMessage(AstExpr* primary)
    {
        AstSymbol* method = ParseBinarySelector();
        AstExpr* op = ParseBinaryOperand();
        AstBinaryCall* res = m_alloc.AllocateObject<AstBinaryCall>();
        res->m_receiver = primary;
        res->m_selector = method;
        res->m_argument = op;
        return res;
    }

    std::string WARN_UNUSED ParseKeyword()
    {
        std::string s = m_curText;
        Expect(Keyword);
        return s;
    }

    AstExpr* WARN_UNUSED ParseKeywordMessageOperand()
    {
        AstExpr* op = ParseBinaryOperand();
        while (m_curSym == OperatorSequence || IsOneOf(binaryOpSyms))
        {
            op = ParseBinaryMessage(op);
        }
        return op;
    }

    AstExpr* WARN_UNUSED ParseKeywordMessage(AstExpr* primary)
    {
        AstKeywordCall* res = m_alloc.AllocateObject<AstKeywordCall>(m_alloc);
        res->m_receiver = primary;
        std::string kw = ParseKeyword();
        res->m_arguments.push_back(ParseKeywordMessageOperand());
        while (m_curSym == Keyword)
        {
            kw.append(ParseKeyword());
            res->m_arguments.push_back(ParseKeywordMessageOperand());
        }
        AstSymbol* selector = m_alloc.AllocateObject<AstSymbol>();
        selector->m_globalOrd = m_symbolInterner->InternString(kw);
        res->m_selector = selector;
        return res;
    }

    AstExpr* WARN_UNUSED ParseEvaluation()
    {
        AstExpr* primary = ParsePrimary();
        while (IsOneOf(identOrPrimitiveSyms))
        {
            primary = ParseUnaryMessage(primary);
        }
        while (m_curSym == OperatorSequence || IsOneOf(binaryOpSyms))
        {
            primary = ParseBinaryMessage(primary);
        }
        if (m_curSym == Keyword)
        {
            primary = ParseKeywordMessage(primary);
        }
        return primary;
    }

    void ParseUnaryMethodPattern(AstMethod* meth /*out*/)
    {
        meth->m_selectorName = ParseUnarySelector();
        meth->m_kind = AstMethodKind::Unary;
    }

    void ParseBinaryMethodPattern(AstMethod* meth /*out*/)
    {
        meth->m_selectorName = ParseBinarySelector();
        meth->m_params.push_back(ParseVariable());
        meth->m_kind = AstMethodKind::Binary;
    }

    void ParseKeywordMethodPattern(AstMethod* meth /*out*/)
    {
        std::string kw;
        do {
            kw.append(ParseKeyword());
            meth->m_params.push_back(ParseVariable());
        } while (m_curSym == Keyword);
        AstSymbol* selector = m_alloc.AllocateObject<AstSymbol>();
        selector->m_globalOrd = m_symbolInterner->InternString(kw);
        meth->m_selectorName = selector;
        meth->m_kind = AstMethodKind::Keyword;
    }

    void ParseMethodPattern(AstMethod* meth /*out*/)
    {
        if (IsOneOf(identOrPrimitiveSyms))
        {
            ParseUnaryMethodPattern(meth /*out*/);
            return;
        }
        if (m_curSym == Keyword)
        {
            ParseKeywordMethodPattern(meth /*out*/);
            return;
        }
        ParseBinaryMethodPattern(meth /*out*/);
    }

    AstMethod* WARN_UNUSED ParseMethod()
    {
        AstMethod* res = m_alloc.AllocateObject<AstMethod>(m_alloc);
        ParseMethodPattern(res);
        // fprintf(stderr, "Method %s\n", std::string(res->m_selectorName->Get(m_symbolInterner)).c_str());
        Expect(Equal);
        if (Accept(Primitive))
        {
            res->m_isPrimitive = true;
        }
        else
        {
            Expect(NewTerm);
            m_currentBlock = res;
            ParseLocals(res->m_locals /*out*/);
            ParseBlockExprs(res->m_body /*out*/);
            m_currentBlock = res;
            Expect(EndTerm);
        }
        return res;
    }

    void ParseMethods(TempVector<AstMethod*>& res /*out*/)
    {
        while (IsOneOf(identOrPrimitiveSyms) || m_curSym == Keyword || m_curSym == OperatorSequence || IsOneOf(binaryOpSyms))
        {
            res.push_back(ParseMethod());
        }
    }

    AstClass* WARN_UNUSED ParseClass()
    {
        AstClass* res = m_alloc.AllocateObject<AstClass>(m_alloc);
        {
            AstSymbol* className = m_alloc.AllocateObject<AstSymbol>();
            className->m_globalOrd = m_symbolInterner->InternString(m_curText);
            res->m_name = className;
        }
        Expect(Identifier);
        Expect(Equal);
        if (m_curSym == Identifier)
        {
            if (m_curText != "nil")
            {
                res->m_superClass = SOMCompileFile(m_curText);
            }
            else
            {
                TestAssert(res->m_name->Get(m_symbolInterner) == "Object");
                res->m_superClass = nullptr;
            }
            Expect(Identifier);
        }
        else
        {
            res->m_superClass = SOMCompileFile("Object");
        }
        Expect(NewTerm);

        ParseLocals(res->m_instanceFields /*out*/);
        ParseMethods(res->m_instanceMethods /*out*/);

        if (Accept(Separator))
        {
            ParseLocals(res->m_classFields /*out*/);
            ParseMethods(res->m_classMethods /*out*/);
        }

        Expect(EndTerm);

        return res;
    }

    TempArenaAllocator& m_alloc;
    StringInterner* m_symbolInterner;
    std::string m_fileName;
    Lexer m_lexer;
    Symbol m_curSym;
    std::string m_curText;
    Symbol m_nextSym;
    AstBlock* m_currentBlock;
};
