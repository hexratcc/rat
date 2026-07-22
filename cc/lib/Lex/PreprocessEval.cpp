#include "Lex/PreprocessDetail.h"

namespace rat::cc {
	namespace detail {
		void Eval::fail(const String& m) {
			if(ok) {
				ok = false;
				err = m;
			}
		}

		Val Eval::parsePrimary() {
			const PpToken& c = cur();
			if(c.kind == Pk::Num) {
				Val v = parseNumLit(*c.text);
				++i;
				return v;
			}
			if(c.kind == Pk::Char) {
				Val v;
				v.u = (U64)parseCharConst(*c.text);
				++i;
				return v;
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
			++i;
			return {};
		}

		Val Eval::parseUnary() {
			if(isOp("+")) {
				++i;
				return parseUnary();
			}
			if(isOp("-")) {
				++i;
				Val v = parseUnary();
				v.u = 0 - v.u; // unsigned wrap
				return v;
			}
			if(isOp("!")) {
				++i;
				Val v = parseUnary();
				return Val{v.u ? 0u : 1u, false};
			}
			if(isOp("~")) {
				++i;
				Val v = parseUnary();
				v.u = ~v.u;
				return v;
			}
			return parsePrimary();
		}

		int Eval::prec(const String& op) {
			if(op == "*" || op == "/" || op == "%")
				return 10;
			if(op == "+" || op == "-")
				return 9;
			if(op == "<<" || op == ">>")
				return 8;
			if(op == "<" || op == "<=" || op == ">" || op == ">=")
				return 7;
			if(op == "==" || op == "!=")
				return 6;
			if(op == "&")
				return 5;
			if(op == "^")
				return 4;
			if(op == "|")
				return 3;
			if(op == "&&")
				return 2;
			if(op == "||")
				return 1;
			return -1;
		}

		Val Eval::apply(const String& op, Val a, Val b) {
			B32 u = a.isU || b.isU;
			auto cmp = [](B32 r) { return Val{r ? 1u : 0u, false}; };
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
			if(op == ">>")
				return a.isU ? Val{a.u >> (b.u & kShiftMask), true}
										 : Val{(U64)((I64)a.u >> (b.u & kShiftMask)), false};
			if(op == "<")
				return cmp(u ? a.u < b.u : (I64)a.u < (I64)b.u);
			if(op == "<=")
				return cmp(u ? a.u <= b.u : (I64)a.u <= (I64)b.u);
			if(op == ">")
				return cmp(u ? a.u > b.u : (I64)a.u > (I64)b.u);
			if(op == ">=")
				return cmp(u ? a.u >= b.u : (I64)a.u >= (I64)b.u);
			if(op == "==")
				return cmp(a.u == b.u);
			if(op == "!=")
				return cmp(a.u != b.u);
			if(op == "&")
				return Val{a.u & b.u, u};
			if(op == "^")
				return Val{a.u ^ b.u, u};
			if(op == "|")
				return Val{a.u | b.u, u};
			if(op == "&&")
				return cmp(a.truth() && b.truth());
			if(op == "||")
				return cmp(a.truth() || b.truth());
			return {};
		}

		Val Eval::parseBinary(int minPrec) {
			Val left = parseUnary();
			while(cur().kind == Pk::Punct) {
				int p = prec(*cur().text);
				if(p < minPrec || p < 0)
					break;
				String op = *cur().text;
				++i;
				if(op == "&&" || op == "||") {
					B32 decide = (op == "&&") ? !left.truth() : left.truth();
					B32 saved = live;
					if(decide)
						live = false;
					Val right = parseBinary(p + 1);
					live = saved;
					left = apply(op, left, right);
				} else {
					Val right = parseBinary(p + 1);
					left = apply(op, left, right);
				}
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

		List<PpToken> Preprocessor::replaceDefined(const List<PpToken>& in) {
			List<PpToken> r;
			size_t i = 0, n = in.size();
			while(i < n) {
				const PpToken& t = in[i];
				if(t.kind == Pk::Id && t.text == idDefined) {
					const String* name = nullptr;
					if(i + 1 < n && isPunct(in[i + 1], "(")) {
						if(i + 3 < n && in[i + 2].kind == Pk::Id && isPunct(in[i + 3], ")")) {
							name = in[i + 2].text;
							i += 4;
						} else {
							fail("malformed 'defined' operator");
							return r;
						}
					} else if(i + 1 < n && in[i + 1].kind == Pk::Id) {
						name = in[i + 1].text;
						i += 2;
					} else {
						fail("malformed 'defined' operator");
						return r;
					}
					r.push_back(makeNum(isDefined(name) ? 1 : 0));
					continue;
				}
				r.push_back(t);
				++i;
			}
			return r;
		}

		B32 Preprocessor::evalExpr(const List<PpToken>& toks) {
			List<PpToken> e = replaceDefined(toks);
			if(!ok)
				return false;
			e = expand(e);
			for(PpToken& t : e)
				if(t.kind == Pk::Id)
					t = makeNum(0);
			PpToken eof;
			eof.kind = Pk::Eof;
			e.push_back(eof);
			if(e.size() == 1) {
				fail("#if with no expression");
				return false;
			}
			Eval ev(e, err);
			Val v = ev.run();
			if(!ev.ok)
				ok = false;
			return v.truth();
		}
	} // namespace detail
} // namespace rat::cc
