#include "lex/preprocess_detail.h"

namespace rat::cc {
	namespace detail {
		void Eval::fail(const String& m) {
			if(ok) {
				ok = false;
				pp.ok = false;
				pp.err = m;
			}
		}

		Val Eval::parsePrimary() {
			const PpToken& c = cur();
			if(c.kind == Pk::Num) {
				++i;
				return parseNumLit(*c.text);
			}
			if(c.kind == Pk::Char) {
				++i;
				return Val{(U64)parseCharConst(*c.text), false};
			}
			if(isOp("(")) {
				++i;
				Val v = parseExpr();
				if(!isOp(")"))
					fail("expected ')' in #if expression");
				else
					++i;
				return v;
			}
			fail("unexpected token in #if expression");
			if(c.kind != Pk::Eof)
				++i;
			return {};
		}

		Val Eval::parseUnary() {
			if(!isOp("+") && !isOp("-") && !isOp("!") && !isOp("~"))
				return parsePrimary();
			C8 op = (*cur().text)[0];
			++i;
			Val v = parseUnary();
			if(op == '-')
				v.u = 0 - v.u; // unsigned wrap
			else if(op == '!')
				v = truthVal(!v.truth());
			else if(op == '~')
				v.u = ~v.u;
			return v;
		}

		I32 Eval::prec(const String& op) {
			struct OpPrec {
				const C8* op;
				I32 prec;
			};
			// clang-format off
			static const OpPrec kPrec[] = {
					{"*", 10}, {"/", 10}, {"%", 10}, {"+", 9}, {"-", 9}, {"<<", 8}, {">>", 8},
					{"<", 7}, {"<=", 7}, {">", 7}, {">=", 7}, {"==", 6}, {"!=", 6}, {"&", 5},
					{"^", 4}, {"|", 3}, {"&&", 2}, {"||", 1},
			};
			// clang-format on
			for(const auto& [spelling, p] : kPrec)
				if(op == spelling)
					return p;
			return -1;
		}

		Val truthVal(B32 r) { return Val{r ? 1u : 0u, false}; }

		Val Eval::apply(const String& op, Val a, Val b) {
			B32 u = a.isU || b.isU;
			if(op == "*")
				return Val{a.u * b.u, u};
			if(op == "+")
				return Val{a.u + b.u, u};
			if(op == "-")
				return Val{a.u - b.u, u};
			if(op == "/" || op == "%") {
				if(b.u == 0) {
					if(live)
						fail("division by zero in #if expression");
					return {};
				}
				if(!u && (I64)a.u == INT64_MIN && (I64)b.u == -1) {
					if(live)
						fail("signed overflow in #if expression");
					return {};
				}
				if(op == "/")
					return u ? Val{a.u / b.u, true} : Val{(U64)((I64)a.u / (I64)b.u), false};
				return u ? Val{a.u % b.u, true} : Val{(U64)((I64)a.u % (I64)b.u), false};
			}
			constexpr U64 kShiftMask = 63;
			if(op == "<<")
				return Val{a.u << (b.u & kShiftMask), a.isU};
			if(op == ">>") {
				U64 shift = b.u & kShiftMask;
				return a.isU ? Val{a.u >> shift, true} : Val{(U64)((I64)a.u >> shift), false};
			}
			if(op == "<")
				return truthVal(u ? a.u < b.u : (I64)a.u < (I64)b.u);
			if(op == "<=")
				return truthVal(u ? a.u <= b.u : (I64)a.u <= (I64)b.u);
			if(op == ">")
				return truthVal(u ? a.u > b.u : (I64)a.u > (I64)b.u);
			if(op == ">=")
				return truthVal(u ? a.u >= b.u : (I64)a.u >= (I64)b.u);
			if(op == "==")
				return truthVal(a.u == b.u);
			if(op == "!=")
				return truthVal(a.u != b.u);
			if(op == "&")
				return Val{a.u & b.u, u};
			if(op == "^")
				return Val{a.u ^ b.u, u};
			if(op == "|")
				return Val{a.u | b.u, u};
			if(op == "&&")
				return truthVal(a.truth() && b.truth());
			if(op == "||")
				return truthVal(a.truth() || b.truth());
			return {};
		}

		Val Eval::parseBinary(I32 minPrec) {
			Val left = parseUnary();
			while(cur().kind == Pk::Punct) {
				I32 p = prec(*cur().text);
				if(p < minPrec || p < 0)
					break;
				String op = *cur().text;
				++i;
				// short-circuit: the dead operand is parsed but not diagnosed
				B32 saved = live;
				if(op == "&&")
					live = saved && left.truth();
				else if(op == "||")
					live = saved && !left.truth();
				Val right = parseBinary(p + 1);
				live = saved;
				left = apply(op, left, right);
			}
			return left;
		}

		Val Eval::parseExpr() {
			Val c = parseBinary(1);
			if(isOp("?")) {
				++i;
				B32 saved = live;
				live = saved && c.truth();
				Val a = parseExpr();
				if(!isOp(":"))
					fail("expected ':' in #if expression");
				else
					++i;
				live = saved && !c.truth();
				Val b = parseExpr();
				live = saved;
				return c.truth() ? a : b;
			}
			return c;
		}

		Val Eval::run() {
			Val v = parseExpr();
			if(ok && !atEnd())
				fail("trailing tokens in #if expression");
			return v;
		}

		List<PpToken> Preprocessor::replaceDefined(PpSpan in) {
			List<PpToken> r;
			r.reserve(in.size());
			U64 i = 0, n = in.size();
			while(i < n) {
				const PpToken& t = in[i];
				if(t.kind == Pk::Id && t.text == idDefined) {
					B32 paren = i + 1 < n && isPunct(in[i + 1], "(");
					U64 nameAt = paren ? i + 2 : i + 1;
					B32 closed = !paren || (nameAt + 1 < n && isPunct(in[nameAt + 1], ")"));
					if(nameAt >= n || in[nameAt].kind != Pk::Id || !closed) {
						fail("malformed 'defined' operator");
						return r;
					}
					r.push_back(makeToken(Pk::Num, isDefined(in[nameAt].text) ? "1" : "0"));
					i = paren ? nameAt + 2 : nameAt + 1;
					continue;
				}
				r.push_back(t);
				++i;
			}
			return r;
		}

		B32 Preprocessor::evalExpr(PpSpan toks) {
			List<PpToken> e = replaceDefined(toks);
			if(!ok)
				return false;
			e = expand(PpSpan(e));
			for(PpToken& t : e)
				if(t.kind == Pk::Id)
					t = makeToken(Pk::Num, "0");
			e.emplace_back(); // Eof
			if(e.size() == 1)
				return fail("#if with no expression");
			Eval ev{e, *this};
			return ev.run().truth();
		}
	} // namespace detail
} // namespace rat::cc
