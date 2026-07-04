#include "Preprocess.h"

#include "CharClass.h"

namespace rat::cc {
	namespace {
		enum class Pk : U8 {
			Eof,
			Id,					// identifier (or keyword)
			Num,				// pp-number
			Char,				// character constant (with optional prefix)
			Str,				// string literal (with optional prefix)
			Punct,			// punctuator
			Placemarker // empty token produced by `##` with an empty operand
		};

		struct PpToken {
			Pk kind = Pk::Eof;
			String text;
			B32 spaceBefore = false; // had white space before it
			B32 bol = false;				 // first token on its logical line
			U32 line = 0;
			String file;
			Set<String> hide; // hide set (macros that must not re-expand here)
		};

		B32 isPunct(const PpToken& t, const char* s) { return t.kind == Pk::Punct && t.text == s; }

		String unquote(const String& s) { return s.size() >= 2 ? s.substr(1, s.size() - 2) : s; }

		size_t ucnLen(const String& s, size_t i) {
			size_t n = s.size();
			if(i + 1 >= n || s[i] != '\\')
				return 0;
			char k = s[i + 1];
			size_t ndig = (k == 'u') ? 4 : (k == 'U') ? 8 : 0;
			if(ndig == 0 || i + 2 + ndig > n)
				return 0;
			for(size_t d = 0; d < ndig; ++d)
				if(!isHexDigit(s[i + 2 + d]))
					return 0;
			return 2 + ndig;
		}

		Pk classify(const String& s) {
			if(s.empty())
				return Pk::Punct;
			if(isIdentStart(s[0]) || ucnLen(s, 0)) {
				for(size_t i = 0; i < s.size();) {
					if(size_t u = ucnLen(s, i)) {
						i += u;
					} else if(isIdentCont(s[i])) {
						++i;
					} else {
						return Pk::Punct;
					}
				}
				return Pk::Id;
			}
			if(isDigit(s[0]) || (s[0] == '.' && s.size() > 1 && isDigit(s[1])))
				return Pk::Num;
			return Pk::Punct;
		}

		// trigraph replacement
		String trigraph(const String& src) {
			String out;
			out.reserve(src.size());
			size_t i = 0, n = src.size();
			while(i < n) {
				if(i + 2 < n && src[i] == '?' && src[i + 1] == '?') {
					char r;
					switch(src[i + 2]) {
					case '=':
						r = '#';
						break;
					case '(':
						r = '[';
						break;
					case '/':
						r = '\\';
						break;
					case ')':
						r = ']';
						break;
					case '\'':
						r = '^';
						break;
					case '<':
						r = '{';
						break;
					case '!':
						r = '|';
						break;
					case '>':
						r = '}';
						break;
					case '-':
						r = '~';
						break;
					default:
						r = 0;
						break;
					}
					if(r) {
						out.push_back(r);
						i += 3;
						continue;
					}
				}
				out.push_back(src[i++]);
			}
			return out;
		}

		// line splicing + newline norm
		void splice(const String& src, String& out, List<U32>& lineOf) {
			U32 line = 1;
			size_t i = 0, n = src.size();
			while(i < n) {
				char c = src[i];
				if(c == '\\' && i + 1 < n &&
					 (src[i + 1] == '\n' || (src[i + 1] == '\r' && i + 2 < n && src[i + 2] == '\n'))) {
					i += (src[i + 1] == '\r') ? 3 : 2;
					++line;
					continue;
				}
				if(c == '\r') {
					if(i + 1 < n && src[i + 1] == '\n')
						++i;
					out.push_back('\n');
					lineOf.push_back(line);
					++line;
					++i;
					continue;
				}
				out.push_back(c);
				lineOf.push_back(line);
				if(c == '\n')
					++line;
				++i;
			}
			if(out.empty() || out.back() != '\n') {
				out.push_back('\n');
				lineOf.push_back(line);
			}
		}

		struct LexResult {
			List<PpToken> toks;
			B32 ok = true;
			String err;
		};

		const char* kPuncts[] = {"%:%:", "...", "<<=", ">>=", "->", "++", "--", "<<", ">>", "<=",
														 ">=",	 "==",	"!=",	 "&&",	"||", "*=", "/=", "%=", "+=", "-=",
														 "&=",	 "|=",	"^=",	 "##",	"<:", ":>", "<%", "%>", "%:"};

		LexResult lexAll(const String& s, const List<U32>& lineOf, const String& file) {
			LexResult r;
			size_t i = 0, n = s.size();
			B32 bolPending = true;
			B32 spacePending = false;

			auto pushTok = [&](Pk kind, size_t start, size_t end) {
				PpToken t;
				t.kind = kind;
				t.text = s.substr(start, end - start);
				if(kind == Pk::Punct) {
					if(t.text == "<:")
						t.text = "[";
					else if(t.text == ":>")
						t.text = "]";
					else if(t.text == "<%")
						t.text = "{";
					else if(t.text == "%>")
						t.text = "}";
					else if(t.text == "%:")
						t.text = "#";
					else if(t.text == "%:%:")
						t.text = "##";
				}
				t.spaceBefore = spacePending;
				t.bol = bolPending;
				t.line = start < lineOf.size() ? lineOf[start] : 0;
				t.file = file;
				r.toks.push_back(std::move(t));
				bolPending = false;
				spacePending = false;
			};

			while(i < n) {
				char c = s[i];
				if(c == '\n') {
					bolPending = true;
					spacePending = true;
					++i;
					continue;
				}
				if(c == ' ' || c == '\t' || c == '\f' || c == '\v') {
					spacePending = true;
					++i;
					continue;
				}
				if(c == '/' && i + 1 < n && s[i + 1] == '/') {
					i += 2;
					while(i < n && s[i] != '\n')
						++i;
					spacePending = true;
					continue;
				}
				if(c == '/' && i + 1 < n && s[i + 1] == '*') {
					i += 2;
					while(i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) {
						if(s[i] == '\n')
							bolPending = true;
						++i;
					}
					if(i + 1 >= n) {
						r.ok = false;
						r.err = "unterminated comment";
						return r;
					}
					i += 2;
					spacePending = true;
					continue;
				}

				// string / char literal, with optional prefix
				size_t pfx = i;
				if(isIdentStart(c) || ucnLen(s, i)) {
					size_t j = i;
					for(;;) {
						if(size_t u = ucnLen(s, j))
							j += u;
						else if(j < n && isIdentCont(s[j]))
							++j;
						else
							break;
					}
					String word = s.substr(i, j - i);
					B32 isPrefix = (word == "L" || word == "u" || word == "U" || word == "u8");
					if(isPrefix && j < n && (s[j] == '"' || s[j] == '\'')) {
						// fall through into literal lexing starting at the quote
						i = j;
						c = s[i];
					} else {
						pushTok(Pk::Id, pfx, j);
						i = j;
						continue;
					}
				}

				if(c == '"' || c == '\'') {
					char quote = c;
					size_t j = i + 1;
					while(j < n && s[j] != quote) {
						if(s[j] == '\\' && j + 1 < n)
							j += 2;
						else if(s[j] == '\n')
							break;
						else
							++j;
					}
					if(j >= n || s[j] != quote) {
						r.ok = false;
						r.err = "unterminated literal";
						return r;
					}
					++j; // include closing quote
					pushTok(quote == '"' ? Pk::Str : Pk::Char, pfx, j);
					i = j;
					continue;
				}

				// pp-number
				if(isDigit(c) || (c == '.' && i + 1 < n && isDigit(s[i + 1]))) {
					size_t j = i + 1;
					while(j < n) {
						char d = s[j];
						if((d == 'e' || d == 'E' || d == 'p' || d == 'P') && j + 1 < n &&
							 (s[j + 1] == '+' || s[j + 1] == '-')) {
							j += 2;
							continue;
						}
						if(isIdentCont(d) || d == '.') {
							++j;
							continue;
						}
						break;
					}
					pushTok(Pk::Num, i, j);
					i = j;
					continue;
				}

				// punctuator
				B32 matched = false;
				for(const char* p : kPuncts) {
					size_t len = std::strlen(p);
					if(i + len <= n && s.compare(i, len, p) == 0) {
						pushTok(Pk::Punct, i, i + len);
						i += len;
						matched = true;
						break;
					}
				}
				if(matched)
					continue;

				pushTok(Pk::Punct, i, i + 1);
				++i;
			}

			PpToken eof;
			eof.kind = Pk::Eof;
			eof.bol = true;
			eof.file = file;
			r.toks.push_back(std::move(eof));
			return r;
		}

		// macros
		struct Macro {
			B32 isFunc = false;
			B32 variadic = false;
			String vaName = "__VA_ARGS__";
			List<String> params; // named parameters (excl variadic)
			List<PpToken> body;
		};

		// constexpr evaluator
		struct Val {
			U64 u = 0;
			B32 isU = false;
			B32 truth() const { return u != 0; }
		};

		I64 parseCharConst(const String& txt) {
			size_t i = 0;
			while(i < txt.size() && txt[i] != '\'')
				++i;
			++i; // skip opening quote
			if(i >= txt.size())
				return 0;
			if(txt[i] != '\\')
				return (I64)(unsigned char)txt[i];
			++i;
			char e = txt[i++];
			U8 simple = 0;
			if(simpleEscape(e, simple))
				return (I64)simple;
			switch(e) {
			case 'x': {
				I64 v = 0;
				while(i < txt.size() && isHexDigit(txt[i]))
					v = v * 16 + hexVal(txt[i++]);
				return v;
			}
			default:
				if(isOctalDigit(e)) {
					I64 v = e - '0';
					for(U32 k = 0; k < 2 && i < txt.size() && isOctalDigit(txt[i]); ++k, ++i)
						v = v * 8 + (txt[i] - '0');
					return v;
				}
				return (I64)(unsigned char)e;
			}
		}

		Val parseNumLit(const String& txt) {
			Val v;
			size_t p = 0;
			int base = 10;
			if(txt.size() >= 2 && txt[0] == '0' && (txt[1] == 'x' || txt[1] == 'X')) {
				base = 16;
				p = 2;
			} else if(txt.size() >= 1 && txt[0] == '0') {
				base = 8;
				p = 1;
			}
			U64 acc = 0;
			for(; p < txt.size(); ++p) {
				int d = hexVal(txt[p]);
				if(d < 0 || d >= base)
					break;
				acc = acc * (U64)base + (U64)d;
			}
			for(; p < txt.size(); ++p) {
				char c = (char)std::tolower(txt[p]);
				if(c == 'u')
					v.isU = true;
			}
			v.u = acc;
			return v;
		}

		struct Eval {
			const List<PpToken>& t;
			size_t i = 0;
			String& err;
			B32 ok = true;
			B32 live = true;

			Eval(const List<PpToken>& toks, String& e)
			: t(toks),
				err(e) {}

			const PpToken& cur() { return t[i]; }
			B32 isOp(const char* s) { return isPunct(cur(), s); }
			B32 atEnd() { return cur().kind == Pk::Eof; }

			void fail(const String& m) {
				if(ok) {
					ok = false;
					err = m;
				}
			}

			Val parsePrimary() {
				const PpToken& c = cur();
				if(c.kind == Pk::Num) {
					Val v = parseNumLit(c.text);
					++i;
					return v;
				}
				if(c.kind == Pk::Char) {
					Val v;
					v.u = (U64)parseCharConst(c.text);
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

			Val parseUnary() {
				if(isOp("+")) {
					++i;
					return parseUnary();
				}
				if(isOp("-")) {
					++i;
					Val v = parseUnary();
					v.u = (U64)(-(I64)v.u);
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

			int prec(const String& op) {
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

			Val apply(const String& op, Val a, Val b) {
				B32 u = a.isU || b.isU;
				auto cmp = [&](B32 r) { return Val{r ? 1u : 0u, false}; };
				if(op == "*")
					return u ? Val{a.u * b.u, true} : Val{(U64)((I64)a.u * (I64)b.u), false};
				if(op == "+")
					return u ? Val{a.u + b.u, true} : Val{(U64)((I64)a.u + (I64)b.u), false};
				if(op == "-")
					return u ? Val{a.u - b.u, true} : Val{(U64)((I64)a.u - (I64)b.u), false};
				if(op == "/" || op == "%") {
					if(b.u == 0) {
						if(live)
							fail("division by zero in #if expression");
						return {};
					}
					if(op == "/")
						return u ? Val{a.u / b.u, true} : Val{(U64)((I64)a.u / (I64)b.u), false};
					return u ? Val{a.u % b.u, true} : Val{(U64)((I64)a.u % (I64)b.u), false};
				}
				if(op == "<<")
					return Val{a.u << (b.u & 63), a.isU};
				if(op == ">>")
					return a.isU ? Val{a.u >> (b.u & 63), true} : Val{(U64)((I64)a.u >> (b.u & 63)), false};
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

			Val parseBinary(int minPrec) {
				Val left = parseUnary();
				while(cur().kind == Pk::Punct) {
					int p = prec(cur().text);
					if(p < minPrec || p < 0)
						break;
					String op = cur().text;
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

			Val parseExpr() {
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

			Val run() {
				Val v = parseExpr();
				if(ok && !atEnd())
					fail("trailing tokens in #if expression");
				return v;
			}
		};

		struct Preprocessor {
			const PpOptions& opts;
			Map<String, Macro> macros;
			List<PpToken> out;
			Set<String> pragmaOnce;
			struct SavedMacro {
				B32 defined;
				Macro macro;
			};
			Map<String, List<SavedMacro>> macroStack;
			U32 includeDepth = 0;
			String err;
			B32 ok = true;
			I64 lineDelta = 0;
			String fileName;

			Preprocessor(const PpOptions& o)
			: opts(o) {}

			void fail(const String& m) {
				if(ok) {
					ok = false;
					err = m;
				}
			}

			static PpToken makeNum(U64 v) {
				PpToken t;
				t.kind = Pk::Num;
				t.text = std::to_string(v);
				return t;
			}
			static PpToken makePunct(const String& s) {
				PpToken t;
				t.kind = Pk::Punct;
				t.text = s;
				return t;
			}

			List<PpToken> lexFragment(const String& text, const String& file) {
				String spliced;
				List<U32> lineOf;
				splice(trigraph(text), spliced, lineOf);
				LexResult lr = lexAll(spliced, lineOf, file);
				if(!lr.ok) {
					fail(file + ": " + lr.err);
					return {};
				}
				lr.toks.pop_back(); // drop Eof
				return lr.toks;
			}

			B32 isBuiltinDynamic(const String& name) { return name == "__LINE__" || name == "__FILE__"; }
			B32 isDefined(const String& name) { return macros.count(name) || isBuiltinDynamic(name); }

			// macro expansion
			static void pasteInto(PpToken& dst, const PpToken& r) {
				if(dst.kind == Pk::Placemarker) {
					B32 sp = dst.spaceBefore;
					dst = r;
					dst.spaceBefore = sp;
					dst.hide.clear();
					return;
				}
				if(r.kind == Pk::Placemarker)
					return;
				dst.text += r.text;
				dst.kind = classify(dst.text);
				dst.hide.clear();
			}

			PpToken stringize(const List<PpToken>& a, B32 spaceBefore) {
				String s = "\"";
				for(size_t k = 0; k < a.size(); ++k) {
					const PpToken& t = a[k];
					if(k > 0 && t.spaceBefore)
						s += ' ';
					if(t.kind == Pk::Str || t.kind == Pk::Char) {
						for(char c : t.text) {
							if(c == '"' || c == '\\')
								s += '\\';
							s += c;
						}
					} else {
						s += t.text;
					}
				}
				s += '"';
				PpToken out;
				out.kind = Pk::Str;
				out.text = s;
				out.spaceBefore = spaceBefore;
				return out;
			}

			void appendList(List<PpToken>& os, List<PpToken> src, B32 firstSpace) {
				for(size_t k = 0; k < src.size(); ++k) {
					src[k].bol = false;
					if(k == 0)
						src[k].spaceBefore = firstSpace;
					os.push_back(std::move(src[k]));
				}
			}

			List<PpToken> substitute(const Macro& m,
															 const List<List<PpToken>>& args,
															 const Set<String>& hs,
															 const List<String>& formals) {
				List<PpToken> os;
				const List<PpToken>& body = m.body;
				auto idxOf = [&](const String& s) -> int {
					for(size_t k = 0; k < formals.size(); ++k)
						if(formals[k] == s)
							return (int)k;
					return -1;
				};
				auto isVa = [&](const String& s) { return m.variadic && s == m.vaName; };

				size_t i = 0;
				while(i < body.size()) {
					const PpToken& T = body[i];
					B32 isHash = isPunct(T, "#");
					B32 isPaste = isPunct(T, "##");

					// # param -> stringize
					if(m.isFunc && isHash && i + 1 < body.size() && idxOf(body[i + 1].text) >= 0) {
						int p = idxOf(body[i + 1].text);
						os.push_back(stringize(args[p], T.spaceBefore));
						i += 2;
						continue;
					}

					// ## token -> paste onto the previous token
					if(isPaste && !os.empty() && i + 1 < body.size()) {
						const PpToken& R = body[i + 1];
						int p = idxOf(R.text);
						if(p >= 0) {
							const List<PpToken>& a = args[p];
							// GNU comma elision
							B32 commaVa = isVa(R.text) && isPunct(os.back(), ",");
							if(commaVa) {
								if(a.empty())
									os.pop_back(); // drop the comma
								else
									appendList(os, a, true); // keep comma, no paste
							} else if(a.empty()) {
								// paste with empty operand -> previous unchanged
							} else {
								pasteInto(os.back(), a.front());
								for(size_t k = 1; k < a.size(); ++k)
									os.push_back(a[k]);
							}
						} else {
							pasteInto(os.back(), R);
						}
						i += 2;
						continue;
					}

					int p = idxOf(T.text);
					if(p >= 0) {
						B32 nextPaste = i + 1 < body.size() && isPunct(body[i + 1], "##");
						if(nextPaste) {
							if(args[p].empty()) {
								PpToken pm;
								pm.kind = Pk::Placemarker;
								pm.spaceBefore = T.spaceBefore;
								os.push_back(pm);
							} else {
								appendList(os, args[p], T.spaceBefore);
							}
						} else {
							appendList(os, expand(args[p]), T.spaceBefore);
						}
						i += 1;
						continue;
					}

					os.push_back(T);
					i += 1;
				}

				// drop placemarkers, then union the hide set into every token
				List<PpToken> res;
				for(PpToken& t : os) {
					if(t.kind == Pk::Placemarker)
						continue;
					for(const String& h : hs)
						t.hide.insert(h);
					res.push_back(std::move(t));
				}
				return res;
			}

			B32 gatherArgs(std::deque<PpToken>& work, List<List<PpToken>>& raw, PpToken& rparen) {
				int depth = 1;
				List<PpToken> cur;
				for(;;) {
					if(work.empty() || work.front().kind == Pk::Eof) {
						fail("unterminated macro argument list");
						return false;
					}
					PpToken w = work.front();
					work.pop_front();
					if(isPunct(w, "(")) {
						++depth;
						cur.push_back(w);
					} else if(isPunct(w, ")")) {
						--depth;
						if(depth == 0) {
							rparen = w;
							raw.push_back(cur);
							return true;
						}
						cur.push_back(w);
					} else if(isPunct(w, ",") && depth == 1) {
						raw.push_back(cur);
						cur.clear();
					} else {
						cur.push_back(w);
					}
				}
			}

			B32 mapArgs(const Macro& m, const List<List<PpToken>>& raw, List<List<PpToken>>& actuals) {
				size_t np = m.params.size();
				if(!m.variadic) {
					if(np == 0 && raw.size() == 1 && raw[0].empty())
						return true;
					if(raw.size() != np) {
						fail("macro invoked with wrong number of arguments");
						return false;
					}
					actuals = raw;
					return true;
				}
				if(np == 0 && raw.size() == 1 && raw[0].empty()) {
					actuals.push_back({});
					return true;
				}
				if(raw.size() < np) {
					fail("macro invoked with too few arguments");
					return false;
				}
				for(size_t k = 0; k < np; ++k)
					actuals.push_back(raw[k]);
				List<PpToken> va;
				for(size_t j = np; j < raw.size(); ++j) {
					if(j > np)
						va.push_back(makePunct(","));
					for(const PpToken& t : raw[j])
						va.push_back(t);
				}
				actuals.push_back(va);
				return true;
			}

			void requeueExpansion(List<PpToken>& r, const PpToken& invoker, std::deque<PpToken>& work) {
				if(!r.empty()) {
					r.front().spaceBefore = invoker.spaceBefore;
					r.front().bol = invoker.bol;
				}
				for(auto rit = r.rbegin(); rit != r.rend(); ++rit)
					work.push_front(*rit);
			}

			List<PpToken> expand(List<PpToken> in) {
				std::deque<PpToken> work(in.begin(), in.end());
				List<PpToken> os;
				while(!work.empty()) {
					PpToken t = work.front();
					work.pop_front();
					if(t.kind != Pk::Id) {
						os.push_back(t);
						continue;
					}
					auto it = macros.find(t.text);
					if(it == macros.end()) {
						if(t.text == "__LINE__") {
							PpToken n = makeNum((U64)((I64)t.line + lineDelta));
							n.spaceBefore = t.spaceBefore;
							n.bol = t.bol;
							os.push_back(n);
							continue;
						}
						if(t.text == "__FILE__") {
							PpToken n;
							n.kind = Pk::Str;
							n.text = "\"" + (fileName.empty() ? t.file : fileName) + "\"";
							n.spaceBefore = t.spaceBefore;
							n.bol = t.bol;
							os.push_back(n);
							continue;
						}
						os.push_back(t);
						continue;
					}
					if(t.hide.count(t.text)) {
						os.push_back(t);
						continue;
					}
					const Macro& m = it->second;
					if(!m.isFunc) {
						Set<String> hs = t.hide;
						hs.insert(t.text);
						List<PpToken> r = substitute(m, {}, hs, {});
						requeueExpansion(r, t, work);
						continue;
					}
					// function-like: requires a paren next
					if(work.empty() || !isPunct(work.front(), "(")) {
						os.push_back(t);
						continue;
					}
					work.pop_front(); // )
					List<List<PpToken>> raw;
					PpToken rparen;
					if(!gatherArgs(work, raw, rparen))
						return os;
					List<List<PpToken>> actuals;
					if(!mapArgs(m, raw, actuals))
						return os;
					List<String> formals = m.params;
					if(m.variadic)
						formals.push_back(m.vaName);
					Set<String> hs;
					for(const String& h : t.hide)
						if(rparen.hide.count(h))
							hs.insert(h);
					hs.insert(t.text);
					List<PpToken> r = substitute(m, actuals, hs, formals);
					requeueExpansion(r, t, work);
				}
				return os;
			}

			// #if expression
			List<PpToken> replaceDefined(const List<PpToken>& in) {
				List<PpToken> r;
				size_t i = 0, n = in.size();
				while(i < n) {
					const PpToken& t = in[i];
					if(t.kind == Pk::Id && t.text == "defined") {
						String name;
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

			B32 evalExpr(const List<PpToken>& toks) {
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

			// #define
			void doDefine(const List<PpToken>& toks) {
				if(toks.empty() || toks[0].kind != Pk::Id) {
					fail("#define expects a macro name");
					return;
				}
				Macro m;
				String name = toks[0].text;
				if(name == "defined") {
					fail("'defined' cannot be used as a macro name");
					return;
				}
				size_t i = 1;
				if(i < toks.size() && isPunct(toks[i], "(") && !toks[i].spaceBefore) {
					m.isFunc = true;
					++i; // (
					B32 expectName = true;
					while(i < toks.size() && !isPunct(toks[i], ")")) {
						const PpToken& t = toks[i];
						if(isPunct(t, ",")) {
							expectName = true;
							++i;
							continue;
						}
						if(isPunct(t, "...")) {
							m.variadic = true;
							m.vaName = "__VA_ARGS__";
							++i;
							break;
						}
						if(t.kind == Pk::Id && expectName) {
							// GNU named variadic
							if(i + 1 < toks.size() && isPunct(toks[i + 1], "...")) {
								m.variadic = true;
								m.vaName = t.text;
								i += 2;
								break;
							}
							m.params.push_back(t.text);
							expectName = false;
							++i;
							continue;
						}
						fail("malformed macro parameter list");
						return;
					}
					if(i >= toks.size() || !isPunct(toks[i], ")")) {
						fail("missing ')' in macro parameter list");
						return;
					}
					++i; // )
				}
				for(; i < toks.size(); ++i) {
					PpToken b = toks[i];
					b.bol = false;
					if(m.body.empty())
						b.spaceBefore = false;
					m.body.push_back(b);
				}
				if(!m.body.empty() && (isPunct(m.body.front(), "##") || isPunct(m.body.back(), "##"))) {
					fail("'##' cannot appear at either end of a macro expansion");
					return;
				}
				if(m.isFunc) {
					for(size_t k = 0; k < m.body.size(); ++k) {
						if(!isPunct(m.body[k], "#"))
							continue;
						B32 okOperand = false;
						if(k + 1 < m.body.size() && m.body[k + 1].kind == Pk::Id) {
							const String& nm = m.body[k + 1].text;
							if(m.variadic && nm == m.vaName)
								okOperand = true;
							for(const String& p : m.params)
								if(p == nm)
									okOperand = true;
						}
						if(!okOperand) {
							fail("'#' is not followed by a macro parameter");
							return;
						}
					}
				}
				macros[name] = std::move(m);
			}

			void defineSimple(const String& name, const String& value) {
				Macro m;
				m.body = lexFragment(value, "<builtin>");
				for(PpToken& t : m.body)
					t.bol = false;
				if(!m.body.empty())
					m.body.front().spaceBefore = false;
				macros[name] = std::move(m);
			}

			// #include
			static String dirOf(const String& path) {
				size_t s = path.find_last_of('/');
				return s == String::npos ? String() : path.substr(0, s + 1);
			}

			B32 readFile(const String& path, String& content) {
				std::ifstream f(path, std::ios::binary);
				if(!f)
					return false;
				std::ostringstream ss;
				ss << f.rdbuf();
				content = ss.str();
				return true;
			}

			void doInclude(const List<PpToken>& restIn, const String& curDir, B32 next = false) {
				List<PpToken> rest = restIn;
				B32 angled = false;
				String fname;

				auto reconstruct = [&](const List<PpToken>& toks) -> B32 {
					if(toks.empty())
						return false;
					if(toks[0].kind == Pk::Str) {
						fname = unquote(toks[0].text);
						angled = false;
						return true;
					}
					if(isPunct(toks[0], "<")) {
						angled = true;
						fname.clear();
						for(size_t k = 1; k < toks.size(); ++k) {
							if(isPunct(toks[k], ">"))
								return true;
							if(k > 1 && toks[k].spaceBefore)
								fname += ' ';
							fname += toks[k].text;
						}
						return false; // missing >
					}
					return false;
				};

				if(!reconstruct(rest)) {
					// try macro expansion of the operand
					rest = expand(rest);
					if(!reconstruct(rest)) {
						fail("#include expects \"file\" or <file>");
						return;
					}
				}

				size_t startDir = 0;
				if(next) {
					String cur = curDir;
					if(!cur.empty() && cur.back() == '/')
						cur.pop_back();
					for(size_t k = 0; k < opts.includeDirs.size(); ++k) {
						String d = opts.includeDirs[k];
						if(!d.empty() && d.back() == '/')
							d.pop_back();
						if(d == cur) {
							startDir = k + 1;
							break;
						}
					}
				}

				List<String> tries;
				if(!next) {
					if(!angled && !curDir.empty())
						tries.push_back(curDir + fname);
					else if(!angled)
						tries.push_back(fname);
				}
				for(size_t k = startDir; k < opts.includeDirs.size(); ++k) {
					String base = opts.includeDirs[k];
					if(!base.empty() && base.back() != '/')
						base += '/';
					tries.push_back(base + fname);
				}

				String found, content;
				for(const String& p : tries) {
					if(readFile(p, content)) {
						found = p;
						break;
					}
				}
				if(found.empty()) {
					fail("cannot find include file '" + fname + "'");
					return;
				}
				if(pragmaOnce.count(found))
					return;
				if(includeDepth > 200) {
					fail("#include nesting too deep");
					return;
				}
				++includeDepth;
				runFile(found, content);
				--includeDepth;
			}

			// #line
			void doLine(const List<PpToken>& restIn, U32 physicalNextLine) {
				List<PpToken> rest = expand(restIn);
				if(rest.empty() || rest[0].kind != Pk::Num) {
					fail("#line expects a line number");
					return;
				}
				U64 n = parseNumLit(rest[0].text).u;
				lineDelta = (I64)n - (I64)physicalNextLine;
				if(rest.size() > 1 && rest[1].kind == Pk::Str)
					fileName = unquote(rest[1].text);
			}

			// #pragma
			void doPragma(const List<PpToken>& rest, const String& path) {
				if(rest.size() == 1 && rest[0].kind == Pk::Id && rest[0].text == "once") {
					pragmaOnce.insert(path);
				} else if(rest.size() >= 1 && rest[0].kind == Pk::Id &&
									(rest[0].text == "push_macro" || rest[0].text == "pop_macro")) {
					String mname;
					for(size_t k = 1; k < rest.size(); ++k) {
						if(rest[k].kind == Pk::Str) {
							mname = unquote(rest[k].text);
							break;
						}
					}
					if(!mname.empty()) {
						if(rest[0].text == "push_macro") {
							auto it = macros.find(mname);
							SavedMacro sv;
							sv.defined = it != macros.end();
							if(sv.defined)
								sv.macro = it->second;
							macroStack[mname].push_back(std::move(sv));
						} else {
							auto it = macroStack.find(mname);
							if(it != macroStack.end() && !it->second.empty()) {
								SavedMacro sv = std::move(it->second.back());
								it->second.pop_back();
								if(sv.defined)
									macros[mname] = std::move(sv.macro);
								else
									macros.erase(mname);
							}
						}
					}
				}
			}

			String destringize(const String& lit) {
				size_t b = 0, e = lit.size();
				while(b < e && lit[b] != '"')
					++b; // skip optional L prefix
				if(b < e && lit[b] == '"')
					++b;
				if(e > b && lit[e - 1] == '"')
					--e;
				String out;
				for(size_t i = b; i < e; ++i) {
					if(lit[i] == '\\' && i + 1 < e && (lit[i + 1] == '"' || lit[i + 1] == '\\'))
						++i;
					out += lit[i];
				}
				return out;
			}

			List<PpToken> applyPragmaOperators(List<PpToken>& toks, const String& path) {
				List<PpToken> out;
				for(size_t i = 0; i < toks.size();) {
					if(toks[i].kind == Pk::Id && toks[i].text == "_Pragma" && i + 3 < toks.size() &&
						 isPunct(toks[i + 1], "(") && toks[i + 2].kind == Pk::Str &&
						 isPunct(toks[i + 3], ")")) {
						List<PpToken> body = lexFragment(destringize(toks[i + 2].text), path);
						doPragma(body, path);
						i += 4;
						continue;
					}
					out.push_back(std::move(toks[i]));
					++i;
				}
				return out;
			}

			void flush(List<PpToken>& textBuf) {
				if(textBuf.empty())
					return;
				List<PpToken> e = expand(textBuf);
				e = applyPragmaOperators(e, fileName);
				for(PpToken& t : e)
					out.push_back(std::move(t));
				textBuf.clear();
			}

			struct Cond {
				B32 parentActive;
				B32 active;
				B32 taken;
				B32 sawElse = false;
			};

			static B32 condActive(const List<Cond>& stack) {
				return stack.empty() ? true : stack.back().active;
			}

			B32 handleConditional(const String& name, const List<PpToken>& rest, List<Cond>& stack) {
				if(name == "if" || name == "ifdef" || name == "ifndef") {
					B32 parent = condActive(stack);
					B32 cond = false;
					if(parent) {
						if(name == "if") {
							cond = evalExpr(rest);
						} else {
							if(rest.empty() || rest[0].kind != Pk::Id) {
								fail("#" + name + " expects an identifier");
							} else {
								B32 def = isDefined(rest[0].text);
								cond = (name == "ifdef") ? def : !def;
							}
						}
					}
					stack.push_back({parent, parent && cond, parent && cond});
					return true;
				}
				if(name == "elif") {
					if(stack.empty()) {
						fail("#elif without #if");
						return true;
					}
					Cond& c = stack.back();
					if(c.sawElse) {
						fail("#elif after #else");
						return true;
					}
					if(c.parentActive && !c.taken) {
						B32 cond = evalExpr(rest);
						c.active = cond;
						if(cond)
							c.taken = true;
					} else {
						c.active = false;
					}
					return true;
				}
				if(name == "else") {
					if(stack.empty()) {
						fail("#else without #if");
						return true;
					}
					Cond& c = stack.back();
					if(c.sawElse) {
						fail("#else after #else");
						return true;
					}
					c.sawElse = true;
					c.active = c.parentActive && !c.taken;
					c.taken = true;
					return true;
				}
				if(name == "endif") {
					if(stack.empty()) {
						fail("#endif without #if");
						return true;
					}
					stack.pop_back();
					return true;
				}
				return false;
			}

			void runFile(const String& path, const String& source) {
				if(!ok)
					return;

				List<PpToken> toks = lexFragment(source, path);
				if(!ok)
					return;

				I64 savedDelta = lineDelta;
				String savedFile = fileName;
				lineDelta = 0;
				fileName = path;
				String curDir = dirOf(path);

				List<Cond> stack;
				List<PpToken> textBuf;

				size_t i = 0, n = toks.size();
				while(i < n && ok) {
					size_t start = i;
					size_t j = i + 1;
					while(j < n && !toks[j].bol)
						++j;
					i = j;

					const PpToken& first = toks[start];
					B32 isDirective = isPunct(first, "#") && first.bol;

					if(!isDirective) {
						if(condActive(stack))
							for(size_t k = start; k < j; ++k)
								textBuf.push_back(toks[k]);
						continue;
					}

					flush(textBuf);

					size_t d = start + 1;
					if(d >= j) // null directive
						continue;

					const PpToken& dir = toks[d];

					if(dir.kind == Pk::Num) {
						if(condActive(stack)) {
							List<PpToken> rest(toks.begin() + d, toks.begin() + j);
							U32 phys = j < n ? toks[j].line : dir.line + 1;
							doLine(rest, phys);
						}
						continue;
					}

					if(dir.kind != Pk::Id) {
						if(condActive(stack))
							fail("invalid preprocessing directive");
						continue;
					}

					const String& name = dir.text;
					List<PpToken> rest(toks.begin() + d + 1, toks.begin() + j);

					if(handleConditional(name, rest, stack))
						continue;

					if(!condActive(stack))
						continue;

					if(name == "define") {
						doDefine(rest);
					} else if(name == "undef") {
						if(rest.empty() || rest[0].kind != Pk::Id)
							fail("#undef expects an identifier");
						else if(rest[0].text == "defined")
							fail("'defined' cannot be used as a macro name");
						else
							macros.erase(rest[0].text);
					} else if(name == "include") {
						doInclude(rest, curDir);
					} else if(name == "include_next") {
						doInclude(rest, curDir, true);
					} else if(name == "error") {
						String msg;
						for(size_t k = 0; k < rest.size(); ++k) {
							if(k && rest[k].spaceBefore)
								msg += ' ';
							msg += rest[k].text;
						}
						fail(path + ": #error " + msg);
					} else if(name == "pragma") {
						doPragma(rest, path);
					} else if(name == "line") {
						U32 phys = j < n ? toks[j].line : dir.line + 1;
						doLine(rest, phys);
					} else {
						fail("invalid preprocessing directive #" + name);
					}
				}

				if(ok && !stack.empty())
					fail(path + ": unterminated #if");

				flush(textBuf);

				lineDelta = savedDelta;
				fileName = savedFile;
			}

			void installBuiltins() {
				time_t now = time(nullptr);
				struct tm* lt = std::localtime(&now);
				char buf[64];
				std::strftime(buf, sizeof buf, "%b %e %Y", lt);
				defineSimple("__DATE__", String("\"") + buf + "\"");
				std::strftime(buf, sizeof buf, "%H:%M:%S", lt);
				defineSimple("__TIME__", String("\"") + buf + "\"");
				defineSimple("__STDC__", "1");
				defineSimple("__STDC_HOSTED__", "1");
				defineSimple("__STDC_VERSION__", "199901L");

				// GNU C extensions
				auto defineFrag = [&](const char* text) { doDefine(lexFragment(text, "<builtin>")); };
				defineFrag("__attribute__(x)");
				defineFrag("__attribute(x)");
				defineFrag("__asm__(x)");
				defineFrag("__asm(x)");
				defineFrag("__restrict");
				defineFrag("__restrict__ restrict");
				defineFrag("__inline inline");
				defineFrag("__inline__ inline");
				defineFrag("__volatile__ volatile");
				defineFrag("__volatile volatile");
				defineFrag("__extension__");
				defineFrag("__signed__ signed");
				defineFrag("__signed signed");
				defineFrag("__const const");
				defineFrag("__thread");
				// GCC extended floating types
				defineFrag("_Float32 float");
				defineFrag("_Float32x double");
				defineFrag("_Float64 double");
				defineFrag("_Float64x double");
				defineFrag("_Float128 double");
				defineFrag("_Float128x double");
				// GCC 128-bit integers
				defineFrag("__int128 long long");
			}

			void applyCommandLine() {
				for(const String& d : opts.defines) {
					size_t eq = d.find('=');
					String left = eq == String::npos ? d : d.substr(0, eq);
					String right = eq == String::npos ? String("1") : d.substr(eq + 1);
					List<PpToken> all = lexFragment(left, "<command-line>");
					List<PpToken> rt = lexFragment(right, "<command-line>");
					all.insert(all.end(), rt.begin(), rt.end());
					doDefine(all);
				}
				for(const String& u : opts.undefs)
					macros.erase(u);
			}

			String serialize() {
				String s;
				B32 first = true;
				for(const PpToken& t : out) {
					if(t.kind == Pk::Placemarker || t.kind == Pk::Eof)
						continue;
					if(!first) {
						if(t.bol)
							s += '\n';
						else
							s += ' ';
					}
					s += t.text;
					first = false;
				}
				s += '\n';
				return s;
			}
		};
	} // namespace

	B32 preprocess(
			const String& path, const String& source, const PpOptions& opts, String& out, String& err) {
		Preprocessor pp(opts);
		pp.installBuiltins();
		pp.applyCommandLine();
		if(pp.ok)
			pp.runFile(path, source);
		if(!pp.ok) {
			err = pp.err;
			return false;
		}
		out = pp.serialize();
		return true;
	}
} // namespace rat::cc
