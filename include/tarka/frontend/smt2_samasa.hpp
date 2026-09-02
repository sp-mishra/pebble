#pragma once
#include "tarka/frontend/smt2_text.hpp"
#include "languages/samasa/samasa.hpp"

namespace tarka::frontend { namespace samasa_smt2 {
        enum class token : std::uint8_t { eof, symbol, integer, real, string, unknown, lparen, rparen };

        enum class syntax : std::uint8_t { root };

        using punctuation = lang::samasa::operator_trie<
            lang::samasa::operator_token < "(", token::lparen>
        ,
        lang::samasa::operator_token<")", token::rparen>
        ,
        lang::samasa::operator_token<"-", token::unknown>
        ,
        lang::samasa::operator_token<"+", token::unknown>
        ,
        lang::samasa::operator_token<"*", token::unknown>
        ,
        lang::samasa::operator_token<"/", token::unknown>
        ,
        lang::samasa::operator_token<">", token::unknown>
        ,
        lang::samasa::operator_token<"<", token::unknown>
        ,
        lang::samasa::operator_token<"=", token::unknown>
        ,
        lang::samasa::operator_token<"#", token::unknown>
        >;
        using atom = lang::samasa::choice_t<lang::samasa::tok < token::symbol>
        ,
        lang::samasa::tok<token::integer>
        ,
        lang::samasa::tok<token::real>
        ,
        lang::samasa::tok<token::string>
        ,
        lang::samasa::tok<token::unknown>
        ,
        lang::samasa::tok<token::lparen>
        ,
        lang::samasa::tok<token::rparen>
        >;
        using root = lang::samasa::rule<"smt2_tokens", lang::samasa::seq_t<lang::samasa::many_t<atom>,
                                                                           lang::samasa::tok < token::eof>,
                                        lang::samasa::eof>
        >;
        using grammar = lang::samasa::grammar<syntax, token, root, root>;
        inline constexpr lang::samasa::scan_token_kinds<token> kinds{
            token::eof, token::symbol, token::integer, token::real, token::string, token::unknown
        };
    }

    // Samasa owns scanning, token classification, diagnostics, and the parse
    // event stream. The semantic decoder builds frontend::ir from that syntax
    // surface; nested S-expression production lowering is the next grammar
    // extension once recursive rule references land in Samasa.
    [[nodiscard]] inline ir::script parse_smt2_samasa(std::string_view source) {
        auto checked = lang::samasa::parse<samasa_smt2::grammar, lang::samasa::keyword_table<>,
                                           samasa_smt2::punctuation>(source, {}, samasa_smt2::kinds);
        auto result = detail::decode_smt2(source);
        if (!checked.success)
            ir::error(result, ir::diagnostic_code::syntax, {},
                      "Samasa rejected SMT-LIB token stream");
        return result;
    }
}
