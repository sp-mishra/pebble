// =============================================================================
// Tarka — Zero-Overhead Multi-Solver SMT Substrate
// include/tarka/frontend/smt2_parser.hpp
//
// SMT-LIB2 Front-End Parser.
// Parses SMT-LIB2 scripts and executes commands on a Tarka RouterEngine.
// C++23, zero virtual, header-only.
// =============================================================================

#pragma once

#include "tarka/context.hpp"
#include "tarka/tarka.hpp"

#include <cctype>
#include <charconv>
#include <expected>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tarka::frontend {
    enum class TokenType {
        LParen,
        RParen,
        Symbol,
        Numeral,
        HexNumeral,
        BinNumeral,
        String,
        Eof
    };

    struct Token {
        TokenType type;
        std::string_view text;
    };

    class Lexer {
    public:
        explicit Lexer(std::string_view src) : src_(src), pos_(0) {}

        Token next_token() {
            skip_whitespace_and_comments();
            if (pos_ >= src_.size()) return {TokenType::Eof, {}};

            const char c = src_[pos_];
            if (c == '(') { ++pos_; return {TokenType::LParen, src_.substr(pos_ - 1, 1)}; }
            if (c == ')') { ++pos_; return {TokenType::RParen, src_.substr(pos_ - 1, 1)}; }

            const std::size_t start = pos_;
            if (c == '#' && pos_ + 1 < src_.size()) {
                if (src_[pos_ + 1] == 'x' || src_[pos_ + 1] == 'X') {
                    pos_ += 2;
                    while (pos_ < src_.size() && std::isxdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
                    return {TokenType::HexNumeral, src_.substr(start, pos_ - start)};
                }
                if (src_[pos_ + 1] == 'b' || src_[pos_ + 1] == 'B') {
                    pos_ += 2;
                    while (pos_ < src_.size() && (src_[pos_] == '0' || src_[pos_] == '1')) ++pos_;
                    return {TokenType::BinNumeral, src_.substr(start, pos_ - start)};
                }
            }

            if (std::isdigit(static_cast<unsigned char>(c))) {
                while (pos_ < src_.size() && (std::isdigit(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '.')) ++pos_;
                return {TokenType::Numeral, src_.substr(start, pos_ - start)};
            }

            // Symbol
            while (pos_ < src_.size() && !std::isspace(static_cast<unsigned char>(src_[pos_])) &&
                   src_[pos_] != '(' && src_[pos_] != ')' && src_[pos_] != ';') {
                ++pos_;
            }
            return {TokenType::Symbol, src_.substr(start, pos_ - start)};
        }

    private:
        void skip_whitespace_and_comments() {
            while (pos_ < src_.size()) {
                if (std::isspace(static_cast<unsigned char>(src_[pos_]))) {
                    ++pos_;
                } else if (src_[pos_] == ';') {
                    while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
                } else {
                    break;
                }
            }
        }

        std::string_view src_;
        std::size_t pos_;
    };

    template <SmtSolverBackend... Backends>
    class smt2_parser {
    public:
        explicit smt2_parser(Context& ctx, RouterEngine<Backends...>& solver)
            : ctx_(ctx), solver_(solver) {}

        [[nodiscard]] std::expected<void, std::string> parse_script(std::string_view smt2_text) {
            Lexer lexer(smt2_text);
            Token tok = lexer.next_token();

            while (tok.type != TokenType::Eof) {
                if (tok.type != TokenType::LParen) {
                    return std::unexpected("Expected '(' at start of command");
                }

                Token cmd = lexer.next_token();
                if (cmd.type != TokenType::Symbol) {
                    return std::unexpected("Expected command symbol");
                }

                if (cmd.text == "set-logic") {
                    skip_sexp(lexer);
                } else if (cmd.text == "declare-const") {
                    Token name_tok = lexer.next_token();
                    auto s = parse_sort(lexer);
                    if (!s) return std::unexpected(s.error());
                    Term sym = ctx_.make_symbol(name_tok.text, *s);
                    symbols_[std::string(name_tok.text)] = sym;
                    consume_rparen(lexer);
                } else if (cmd.text == "declare-fun") {
                    Token name_tok = lexer.next_token();
                    // parse domain args
                    Token lp = lexer.next_token();
                    std::vector<Sort> domain;
                    while (true) {
                        Token peek = lexer.next_token();
                        if (peek.type == TokenType::RParen) break;
                        if (peek.type == TokenType::Symbol) {
                            domain.push_back(resolve_sort_name(peek.text));
                        }
                    }
                    auto ret_s = parse_sort(lexer);
                    if (!ret_s) return std::unexpected(ret_s.error());

                    Sort fn_sort = domain.empty() ? *ret_s : ctx_.function_sort(domain, *ret_s);
                    Term sym = ctx_.make_symbol(name_tok.text, fn_sort);
                    symbols_[std::string(name_tok.text)] = sym;
                    consume_rparen(lexer);
                } else if (cmd.text == "assert") {
                    auto t = parse_term(lexer);
                    if (!t) return std::unexpected(t.error());
                    solver_.assert_formula(*t);
                    consume_rparen(lexer);
                } else if (cmd.text == "check-sat") {
                    last_result_ = solver_.check_sat();
                    consume_rparen(lexer);
                } else if (cmd.text == "push") {
                    solver_.push();
                    consume_rparen(lexer);
                } else if (cmd.text == "pop") {
                    solver_.pop();
                    consume_rparen(lexer);
                } else if (cmd.text == "exit") {
                    consume_rparen(lexer);
                    break;
                } else {
                    skip_sexp(lexer);
                }

                tok = lexer.next_token();
            }

            return {};
        }

        [[nodiscard]] std::expected<SatResult, SmtError> last_result() const noexcept {
            return last_result_;
        }

    private:
        void consume_rparen(Lexer& lexer) {
            Token t = lexer.next_token();
            (void)t;
        }

        void skip_sexp(Lexer& lexer) {
            int depth = 1;
            while (depth > 0) {
                Token t = lexer.next_token();
                if (t.type == TokenType::Eof) break;
                if (t.type == TokenType::LParen) ++depth;
                else if (t.type == TokenType::RParen) --depth;
            }
        }

        [[nodiscard]] Sort resolve_sort_name(std::string_view name) {
            if (name == "Bool") return ctx_.bool_sort();
            if (name == "Int") return ctx_.int_sort();
            if (name == "Real") return ctx_.real_sort();
            return ctx_.string_sort();
        }

        [[nodiscard]] std::expected<Sort, std::string> parse_sort(Lexer& lexer) {
            Token tok = lexer.next_token();
            if (tok.type == TokenType::Symbol) {
                return resolve_sort_name(tok.text);
            }
            if (tok.type == TokenType::LParen) {
                Token head = lexer.next_token();
                if (head.text == "_" || head.text == "BitVec") {
                    Token bv_name = lexer.next_token();
                    Token width_tok = lexer.next_token();
                    std::uint32_t width = 32;
                    std::from_chars(width_tok.text.data(), width_tok.text.data() + width_tok.text.size(), width);
                    consume_rparen(lexer);
                    return ctx_.bv_sort(width);
                }
                if (head.text == "Array") {
                    auto idx = parse_sort(lexer);
                    auto elem = parse_sort(lexer);
                    consume_rparen(lexer);
                    return ctx_.array_sort(*idx, *elem);
                }
                skip_sexp(lexer);
                return ctx_.bool_sort();
            }
            return std::unexpected("Invalid sort token");
        }

        [[nodiscard]] std::expected<Term, std::string> parse_term(Lexer& lexer) {
            Token tok = lexer.next_token();
            return parse_term_with_token(lexer, tok);
        }

        [[nodiscard]] std::expected<Term, std::string> parse_term_with_token(Lexer& lexer, Token tok) {
            if (tok.type == TokenType::Symbol) {
                if (tok.text == "true") return ctx_.make_bool(true);
                if (tok.text == "false") return ctx_.make_bool(false);
                if (auto it = symbols_.find(std::string(tok.text)); it != symbols_.end()) {
                    return it->second;
                }
                return ctx_.make_symbol(tok.text, ctx_.bool_sort());
            }

            if (tok.type == TokenType::Numeral) {
                std::int64_t val = 0;
                std::from_chars(tok.text.data(), tok.text.data() + tok.text.size(), val);
                return ctx_.make_int(val, ctx_.int_sort());
            }

            if (tok.type == TokenType::HexNumeral) {
                std::uint64_t val = 0;
                std::string_view hex = tok.text.substr(2);
                std::from_chars(hex.data(), hex.data() + hex.size(), val, 16);
                std::uint32_t width = static_cast<std::uint32_t>(hex.size() * 4);
                return ctx_.make_value(val, ctx_.bv_sort(width));
            }

            if (tok.type == TokenType::BinNumeral) {
                std::uint64_t val = 0;
                std::string_view bin = tok.text.substr(2);
                std::from_chars(bin.data(), bin.data() + bin.size(), val, 2);
                std::uint32_t width = static_cast<std::uint32_t>(bin.size());
                return ctx_.make_value(val, ctx_.bv_sort(width));
            }

            if (tok.type == TokenType::LParen) {
                Token op_tok = lexer.next_token();
                std::vector<Term> args;

                while (true) {
                    Token next_tok = lexer.next_token();
                    if (next_tok.type == TokenType::RParen || next_tok.type == TokenType::Eof) {
                        break;
                    }
                    auto arg = parse_term_with_token(lexer, next_tok);
                    if (!arg) return std::unexpected(arg.error());
                    args.push_back(*arg);
                }

                // Op mapping
                if (op_tok.text == "not" && args.size() == 1) return !args[0];
                if (op_tok.text == "and") {
                    Term res = ctx_.make_bool(true);
                    for (Term a : args) res = res && a;
                    return res;
                }
                if (op_tok.text == "or") {
                    Term res = ctx_.make_bool(false);
                    for (Term a : args) res = res || a;
                    return res;
                }
                if (op_tok.text == "=" && args.size() == 2) return (args[0] == args[1]);
                if (op_tok.text == "distinct" && args.size() == 2) return (args[0] != args[1]);
                if (op_tok.text == "bvadd" && args.size() == 2) return ctx_.make_term(Op::BvAdd, args[0].sort(), args);
                if (op_tok.text == "bvsub" && args.size() == 2) return ctx_.make_term(Op::BvSub, args[0].sort(), args);
                if (op_tok.text == "bvmul" && args.size() == 2) return ctx_.make_term(Op::BvMul, args[0].sort(), args);
                if (op_tok.text == "bvudiv" && args.size() == 2) return ctx_.make_term(Op::BvUdiv, args[0].sort(), args);
                if (op_tok.text == "bvurem" && args.size() == 2) return ctx_.make_term(Op::BvUrem, args[0].sort(), args);
                if (op_tok.text == "bvand" && args.size() == 2) return ctx_.make_term(Op::BvAnd, args[0].sort(), args);
                if (op_tok.text == "bvor" && args.size() == 2) return ctx_.make_term(Op::BvOr, args[0].sort(), args);
                if (op_tok.text == "bvxor" && args.size() == 2) return ctx_.make_term(Op::BvXor, args[0].sort(), args);
                if (op_tok.text == "select" && args.size() == 2) {
                    Sort elem_s = args[0].sort().valid() ? ctx_.bv_sort(32) : ctx_.bool_sort();
                    return ctx_.make_term(Op::Select, elem_s, args);
                }
                if (op_tok.text == "store" && args.size() == 3) return ctx_.make_term(Op::Store, args[0].sort(), args);

                // Uninterpreted function application
                if (auto it = symbols_.find(std::string(op_tok.text)); it != symbols_.end()) {
                    std::vector<Term> apply_args;
                    apply_args.push_back(it->second);
                    apply_args.insert(apply_args.end(), args.begin(), args.end());
                    Sort ret_s = ctx_.string_sort();
                    return ctx_.make_term(Op::Apply, ret_s, apply_args);
                }

                return ctx_.make_bool(true);
            }

            return std::unexpected("Failed to parse term");
        }

        Context& ctx_;
        RouterEngine<Backends...>& solver_;
        std::unordered_map<std::string, Term> symbols_;
        std::expected<SatResult, SmtError> last_result_{SatResult::Unknown};
    };
} // namespace tarka::frontend
