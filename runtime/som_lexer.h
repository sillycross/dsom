#pragma once

/*
 *
 *
 Copyright (c) 2007 Michael Haupt, Tobias Pape, Arne Bergmann
 Software Architecture Group, Hasso Plattner Institute, Potsdam, Germany
 http://www.hpi.uni-potsdam.de/swa/

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
*/

#include <istream>
#include <string>
#include "common.h"

class SourceCoordinate {
public:
    SourceCoordinate(size_t line_, size_t column_) : line(line_), column(column_) {}
    SourceCoordinate() : line(0), column(0) {}

    [[nodiscard]] inline size_t GetLine() const { return line; }
    [[nodiscard]] inline size_t GetColumn() const { return column; }

    inline bool operator==(const SourceCoordinate& other) const {
        return line == other.line && column == other.column;
    }

private:
    /* 1-based */
    size_t line;

    /* 1-based */
    size_t column;
};

enum Symbol : uint8_t {
    NONE,
    Integer,
    Double,
    Not,
    And,
    Or,
    Star,
    Div,
    Mod,
    Plus,
    Minus,
    Equal,
    More,
    Less,
    Comma,
    At,
    Per,
    NewBlock,
    EndBlock,
    Colon,
    Period,
    Exit,
    Assign,
    NewTerm,
    EndTerm,
    Pound,
    Primitive,
    Separator,
    STString,
    Identifier,
    Keyword,
    KeywordSequence,
    OperatorSequence
};

// clang-format off
static const char* symnames[] = { "NONE", "Integer", "Not", "And", "Or", "Star",
                                  "Div", "Mod", "Plus", "Minus", "Equal", "More", "Less", "Comma", "At",
                                  "Per", "NewBlock", "EndBlock", "Colon", "Period", "Exit", "Assign",
                                  "NewTerm", "EndTerm", "Pound", "Primitive", "Separator", "STString",
                                  "Identifier", "Keyword", "KeywordSequence", "OperatorSequence" };
// clang-format on

class LexerState {
public:
    LexerState() = default;

    inline size_t incPtr() { return incPtr(1); }

    inline size_t incPtr(size_t val) {
        const size_t cur = bufp;
        bufp += val;
        startBufp = bufp;
        return cur;
    }

    size_t lineNumber{0};
    size_t bufp{0};

    Symbol sym{Symbol::NONE};
    char symc{0};
    std::string text;

    size_t startBufp{0};
};

class Lexer {
public:
    explicit Lexer(std::istream& file);
    explicit Lexer(const std::string& stream);

    Symbol GetSym();
    Symbol Peek();

    [[nodiscard]] std::string GetText() const { return state.text; }

    [[nodiscard]] std::string GetNextText() const {
        return stateAfterPeek.text;
    }

    [[nodiscard]] std::string GetRawBuffer() const {
        // for debug
        return {buf};
    }

    std::string GetCurrentLine();

    [[nodiscard]] size_t GetCurrentColumn() const {
        return state.startBufp + 1 - state.text.length();
    }

    [[nodiscard]] size_t GetCurrentLineNumber() const {
        return state.lineNumber;
    }

    [[nodiscard]] bool GetPeekDone() const { return peekDone; }

    [[nodiscard]] SourceCoordinate GetCurrentSource() const {
        return {GetCurrentLineNumber(), GetCurrentColumn()};
    }

private:
    int64_t fillBuffer();
    void skipWhiteSpace();
    void skipComment();

    bool hasMoreInput();

    void lexNumber();
    void lexOperator();
    void lexEscapeChar();
    void lexStringChar();
    void lexString();

    bool nextWordInBufferIsPrimitive();

    std::istream& infile;

    bool peekDone;

    LexerState state;
    LexerState stateAfterPeek;

    std::string buf;
};
