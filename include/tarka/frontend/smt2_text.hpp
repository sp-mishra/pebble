#pragma once

// Shared SMT-LIB concrete-syntax decoding helpers.  Parser engines supply the
// syntax guarantee; this small lowering decoder turns the engine's source view
// into the frontend-neutral IR.
#include "tarka/frontend/ir.hpp"

#include <charconv>
#include <cctype>
#include <expected>
#include <string_view>

namespace tarka::frontend::detail {
    class text_decoder {
    public:
        explicit text_decoder(std::string_view source) : source_(source) {}

        [[nodiscard]] ir::script run() {
            while (next() != ")") {
                if (eof_) break;
                if (token_ != "(") {
                    bad("expected command");
                    break;
                }
                command();
            }
            return std::move(out_);
        }

    private:
        std::string_view source_;
        std::size_t pos_ = 0;
        std::string_view token_{};
        bool eof_ = false;
        ir::script out_;

        std::string_view next() {
            while (pos_ < source_.size() && (source_[pos_] == ' ' || source_[pos_] == '\n' || source_[pos_] == '\r' ||
                source_[pos_] == '\t'))
                ++pos_;
            if (pos_ < source_.size() && source_[pos_] == ';') {
                while (pos_ < source_.size() && source_[pos_] != '\n')++pos_;
                return next();
            }
            if (pos_ >= source_.size()) {
                eof_ = true;
                return token_ = {};
            }
            auto b = pos_++;
            if (source_[b] == '(' || source_[b] == ')')return token_ = source_.substr(b, 1);
            while (pos_ < source_.size() && !std::isspace(static_cast<unsigned char>(source_[pos_])) && source_[pos_] !=
                '(' && source_[pos_] != ')')
                ++pos_;
            return token_ = source_.substr(b, pos_ - b);
        }

        void bad(std::string m) {
            ir::error(out_, ir::diagnostic_code::syntax, {static_cast<std::uint32_t>(pos_), 0}, std::move(m));
        }

        ir::node_id sort() {
            auto t = next();
            if (t == "Bool")return ir::append(out_, ir::kind::sort_bool);
            if (t == "Int")return ir::append(out_, ir::kind::sort_int);
            if (t == "Real")return ir::append(out_, ir::kind::sort_real);
            if (t == "String")return ir::append(out_, ir::kind::sort_string);
            if (t == "(") {
                auto h = next();
                if (h == "_") {
                    if (next() != "BitVec")bad("expected BitVec");
                    auto w = next();
                    std::uint32_t n{};
                    std::from_chars(w.data(), w.data() + w.size(), n);
                    if (next() != ")")bad("expected )");
                    return ir::append(out_, ir::kind::sort_bitvec, {}, 0, {{n}});
                }
                if (h == "Array") {
                    auto a = sort(), b = sort();
                    if (next() != ")")bad("expected )");
                    ir::node_id c[] = {a, b};
                    return ir::append(out_, ir::kind::sort_array, {}, 0, {}, c);
                }
            }
            bad("unsupported sort");
            return ir::append(out_, ir::kind::sort_bool);
        }

        ir::node_id term_with(std::string_view t) {
            if (t == "true" || t == "false")
                return ir::append(out_, ir::kind::bool_literal, {}, 0,
                                  {{std::uint32_t(t == "true")}});
            if (!t.empty() && t[0] == '#') {
                std::uint64_t v{};
                auto p = t.substr(2);
                std::from_chars(p.data(), p.data() + p.size(), v, t[1] == 'x' ? 16 : 2);
                return ir::append(out_, ir::kind::bv_literal, {}, 0, {
                                      {ir::bit_vector{v, static_cast<std::uint32_t>(p.size() * (t[1] == 'x' ? 4 : 1))}}
                                  });
            }
            std::int64_t i{};
            auto [e,ec] = std::from_chars(t.data(), t.data() + t.size(), i);
            if (ec == std::errc{} && e == t.data() + t.size())
                return ir::append(
                    out_, ir::kind::int_literal, {}, 0, {{i}});
            if (t == "(") {
                auto op = next();
                std::vector<ir::node_id> kids;
                while (next() != ")" && !eof_)kids.push_back(term_with(token_));
                return ir::append(out_, ir::kind::application, {}, out_.symbols.intern(op), {}, kids);
            }
            return ir::append(out_, ir::kind::symbol, {}, out_.symbols.intern(t));
        }

        ir::node_id term() { return term_with(next()); }

        void command() {
            auto c = next();
            if (c == "set-logic") {
                auto n = out_.symbols.intern(next());
                out_.commands.push_back(ir::append(out_, ir::kind::set_logic, {}, n));
                next();
            }
            else if (c == "declare-const") {
                auto n = out_.symbols.intern(next());
                auto s = sort();
                if (next() != ")")bad("expected )");
                ir::node_id k[] = {s};
                out_.commands.push_back(ir::append(out_, ir::kind::declare_const, {}, n, {}, k));
            }
            else if (c == "assert") {
                auto t = term();
                if (next() != ")")bad("expected )");
                ir::node_id k[] = {t};
                out_.commands.push_back(ir::append(out_, ir::kind::assert_, {}, 0, {}, k));
            }
            else if (c == "check-sat") {
                if (next() != ")")bad("expected )");
                out_.commands.push_back(ir::append(out_, ir::kind::check_sat));
            }
            else {
                bad("unsupported SMT-LIB command: " + std::string(c));
                int d = 1;
                while (d && !eof_) {
                    auto x = next();
                    if (x == "(")++d;
                    else if (x == ")")--d;
                }
            }
        }
    };

    [[nodiscard]] inline ir::script decode_smt2(std::string_view text) { return text_decoder{text}.run(); }
}
