#include "lex/preprocess_detail.h"

#include "lex/char_class.h"

namespace rat::cc {
	namespace detail {
		String unquote(const String& s) { return s.size() >= 2 ? s.substr(1, s.size() - 2) : s; }

		B32 isAbsPath(const String& s) {
			if(s.empty())
				return false;
			if(s[0] == '/' || s[0] == '\\')
				return true;
			B32 drive = (s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z');
			return drive && s.size() >= 3 && s[1] == ':' && (s[2] == '/' || s[2] == '\\');
		}

		U64 ucnLen(const String& s, U64 i) {
			U64 n = s.size();
			if(i + 1 >= n || s[i] != '\\')
				return 0;
			C8 k = s[i + 1];
			U64 ndig = (k == 'u') ? 4 : (k == 'U') ? 8 : 0;
			if(ndig == 0 || i + 2 + ndig > n)
				return 0;
			for(U64 d = 0; d < ndig; ++d)
				if(!isHexDigit(s[i + 2 + d]))
					return 0;
			return 2 + ndig;
		}

		Pk classify(const String& s) {
			if(s.empty())
				return Pk::Punct;
			if(isIdentStart(s[0]) || ucnLen(s, 0)) {
				for(U64 i = 0; i < s.size();) {
					if(U64 u = ucnLen(s, i)) {
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

		// trigraph + splice + newline norm in one pass
		void splice(const String& src, String& out, List<LineMark>& marks) {
			U32 line = 1;
			B32 pendingMark = false;
			U64 i = 0, n = src.size();
			out.reserve(n + 1);

			// decode one logical char at p (trigraph-aware); returns length
			auto decode = [&](U64 p, C8& c) -> U64 {
				c = src[p];
				if(c == '?' && p + 2 < n && src[p + 1] == '?') {
					C8 r = 0;
					switch(src[p + 2]) {
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
						break;
					}
					if(r) {
						c = r;
						return 3;
					}
				}
				return 1;
			};

			auto emit = [&](C8 c) {
				if(pendingMark) {
					marks.push_back({(U32)out.size(), line});
					pendingMark = false;
				}
				out.push_back(c);
			};

			const C8* data = src.data();
			while(i < n) {
				// fast path: bulk-copy a run with no splice/trigraph/CR triggers
				U64 start = i;
				U32 startLine = line; // line of first C8 in run
				while(i < n) {
					C8 c = data[i];
					if(c == '\\' || c == '\r' || c == '?')
						break;
					if(c == '\n')
						++line;
					++i;
				}
				if(i > start) {
					if(pendingMark) {
						marks.push_back({(U32)out.size(), startLine});
						pendingMark = false;
					}
					out.append(data + start, i - start);
				}
				if(i >= n)
					break;

				C8 c;
				U64 len = decode(i, c);
				if(c == '\\') {
					// backslash-newline splice (backslash may be a trigraph)
					U64 j = i + len;
					if(j < n) {
						if(src[j] == '\n') {
							i = j + 1;
							++line;
							pendingMark = true;
							continue;
						}
						if(src[j] == '\r' && j + 1 < n && src[j + 1] == '\n') {
							i = j + 2;
							++line;
							pendingMark = true;
							continue;
						}
					}
					emit(c);
					i += len;
					continue;
				}
				if(c == '\r') {
					if(i + 1 < n && src[i + 1] == '\n')
						++i;
					emit('\n');
					++line;
					++i;
					continue;
				}
				emit(c);
				if(c == '\n')
					++line;
				i += len;
			}
			if(out.empty() || out.back() != '\n')
				emit('\n');
		}

		// longest-match punctuator length at s[i]; assumes s[i] starts one
		inline U64 punctLen(const String& s, U64 i, U64 n) {
			C8 c = s[i];
			C8 d = i + 1 < n ? s[i + 1] : '\0';
			C8 e = i + 2 < n ? s[i + 2] : '\0';
			switch(c) {
			case '.':
				return (d == '.' && e == '.') ? 3 : 1;
			case '<':
				if(d == '<')
					return e == '=' ? 3 : 2;
				return (d == '=' || d == ':' || d == '%') ? 2 : 1;
			case '>':
				if(d == '>')
					return e == '=' ? 3 : 2;
				return d == '=' ? 2 : 1;
			case '%':
				if(d == ':')
					return (e == '%' && i + 3 < n && s[i + 3] == ':') ? 4 : 2;
				return (d == '=' || d == '>') ? 2 : 1;
			case ':':
				return d == '>' ? 2 : 1;
			case '#':
				return d == '#' ? 2 : 1;
			case '+':
				return (d == '+' || d == '=') ? 2 : 1;
			case '-':
				return (d == '-' || d == '=' || d == '>') ? 2 : 1;
			case '&':
				return (d == '&' || d == '=') ? 2 : 1;
			case '|':
				return (d == '|' || d == '=') ? 2 : 1;
			case '=':
			case '!':
			case '*':
			case '/':
			case '^':
				return d == '=' ? 2 : 1;
			default:
				return 1;
			}
		}

		LexResult
		lexAll(const String& s, const List<LineMark>& marks, const String* file, Interner& in) {
			LexResult r;
			U64 i = 0, n = s.size();
			r.toks.reserve(n / 3 + 8);
			B32 bolPending = true;
			B32 spacePending = false;
			U32 line = 1;
			U64 mi = 0, mn = marks.size();

			// apply splice line-corrections at offsets <= p
			auto advanceTo = [&](U64 p) {
				while(mi < mn && marks[mi].off <= p) {
					line = marks[mi].line;
					++mi;
				}
			};

			auto pushTok = [&](Pk kind, U64 start, U64 end) {
				PpToken t;
				t.kind = kind;
				std::string_view sv(s.data() + start, end - start);
				if(kind == Pk::Punct) {
					// digraph canonicalization
					if(sv == "<:")
						sv = "[";
					else if(sv == ":>")
						sv = "]";
					else if(sv == "<%")
						sv = "{";
					else if(sv == "%>")
						sv = "}";
					else if(sv == "%:")
						sv = "#";
					else if(sv == "%:%:")
						sv = "##";
				}
				t.text = in.intern(sv);
				t.spaceBefore = spacePending;
				t.bol = bolPending;
				t.line = line;
				t.file = file;
				r.toks.push_back(t);
				bolPending = false;
				spacePending = false;
			};

			while(i < n) {
				advanceTo(i);
				C8 c = s[i];
				if(c == '\n') {
					bolPending = true;
					spacePending = true;
					++line;
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
						if(s[i] == '\n') {
							bolPending = true;
							advanceTo(i);
							++line;
						}
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
				U64 pfx = i;
				if(isIdentStart(c) || ucnLen(s, i)) {
					U64 j = i;
					for(;;) {
						if(U64 u = ucnLen(s, j))
							j += u;
						else if(j < n && isIdentCont(s[j]))
							++j;
						else
							break;
					}
					std::string_view word(s.data() + i, j - i);
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
					C8 quote = c;
					U64 j = i + 1;
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
					U64 j = i + 1;
					while(j < n) {
						C8 d = s[j];
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
				U64 plen = punctLen(s, i, n);
				pushTok(Pk::Punct, i, i + plen);
				i += plen;
			}

			PpToken eof;
			eof.kind = Pk::Eof;
			eof.text = in.intern(std::string_view());
			eof.bol = true;
			eof.file = file;
			r.toks.push_back(eof);
			return r;
		}

		I64 parseCharConst(const String& txt) {
			U64 i = 0;
			while(i < txt.size() && txt[i] != '\'')
				++i;
			++i; // skip opening quote
			if(i >= txt.size())
				return 0;
			if(txt[i] != '\\')
				return (I64)(U8)txt[i];
			++i;
			C8 e = txt[i++];
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
				return (I64)(U8)e;
			}
		}

		Val parseNumLit(const String& txt) {
			Val v;
			U64 p = 0;
			I32 base = 10;
			if(txt.size() >= 2 && txt[0] == '0' && (txt[1] == 'x' || txt[1] == 'X')) {
				base = 16;
				p = 2;
			} else if(txt.size() >= 1 && txt[0] == '0') {
				base = 8;
				p = 1;
			}
			U64 acc = 0;
			for(; p < txt.size(); ++p) {
				I32 d = hexVal(txt[p]);
				if(d < 0 || d >= base)
					break;
				acc = acc * (U64)base + (U64)d;
			}
			for(; p < txt.size(); ++p) {
				C8 c = (C8)std::tolower(txt[p]);
				if(c == 'u')
					v.isU = true;
			}
			v.u = acc;
			return v;
		}
	} // namespace detail
} // namespace rat::cc
