#pragma once

// samasa/samasa_lex.hpp — Lean aggregate: scanner + tokens only.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// For consumers that only need to tokenize source text (syntax highlighters,
// line counters, lexer-only front-ends). Pulls core/ + lex/ and nothing else —
// avoids compiling the grammar, dsl, tree, expr, recovery, and policy layers.
//
// For full parsing (grammars, green/red trees, Pratt expressions, recovery,
// memoization), include "languages/samasa/samasa.hpp" instead.
//
// Usage:
//   #include "languages/samasa/samasa_lex.hpp"
//   using namespace lang::samasa;
//   auto tokens = scan<KWTable, OpTrie, LinePolicy, TokenKind>(source, kinds, lp, sink);

// Core
#include "core/source_view.hpp"
#include "core/cursor.hpp"
#include "core/result.hpp"
#include "core/diagnostic.hpp"
#include "core/limits.hpp"

// Lexer
#include "lex/token.hpp"
#include "lex/token_stream.hpp"
#include "lex/keyword_table.hpp"
#include "lex/operator_trie.hpp"
#include "lex/line_policy.hpp"
#include "lex/scanner.hpp"
