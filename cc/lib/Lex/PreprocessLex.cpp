#include "Lex/PreprocessDetail.h"

#include "Lex/CharClass.h"

namespace rat::cc {
	namespace detail {
		B32 isPunct(const PpToken& t, const char* s) { return t.kind == Pk::Punct && t.text == s; }

		String unquote(const String& s) { return s.size() >= 2 ? s.substr(1, s.size() - 2) : s; }

		void stripTrailingSlash(String& s) {
			if(!s.empty() && s.back() == '/')
				s.pop_back();
		}

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

		const char* kPuncts[] = {"%:%:", "...", "<<=", ">>=", "->", "++", "--", "<<", ">>", "<=",
														 ">=",	 "==",	"!=",	 "&&",	"||", "*=", "/=", "%=", "+=", "-=",
														 "&=",	 "|=",	"^=",	 "##",	"<:", ":>", "<%", "%>", "%:"};

		LexResult lexAll(const String& s, const List<U32>& lineOf, const String* file) {
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
	} // namespace detail
} // namespace rat::cc
