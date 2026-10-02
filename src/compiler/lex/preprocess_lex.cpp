#include "lex/preprocess_detail.h"

#include "lex/char_class.h"

#include <cstring>

#include "hash.h"

namespace rat::cc {
	namespace detail {
		const String* Interner::intern(std::string_view s) {
			U64 h = kFnvBasis;
			for(C8 c : s)
				hashMix(h, (U8)c);
			U64 mask = slots.size() - 1;
			for(U64 i = h & mask;; i = (i + 1) & mask) {
				Slot& sl = slots[i];
				if(!sl.str) {
					store.emplace_back(s);
					sl.hash = h;
					sl.str = &store.back();
					const String* p = sl.str;
					if(++count * 2 > slots.size())
						grow();
					return p;
				}
				if(sl.hash == h && std::string_view(*sl.str) == s)
					return sl.str;
			}
		}

		void Interner::grow() {
			List<Slot> old = std::move(slots);
			slots = List<Slot>(old.size() * 2);
			U64 mask = slots.size() - 1;
			for(const Slot& sl : old) {
				if(!sl.str)
					continue;
				U64 i = sl.hash & mask;
				while(slots[i].str)
					i = (i + 1) & mask;
				slots[i] = sl;
			}
		}

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

		U64 decodeTrigraph(const String& src, U64 p, C8& c) {
			static const C8 kFrom[] = "=(/)'<!>-";
			static const C8 kTo[] = "#[\\]^{|}~";
			c = src[p];
			if(c != '?' || p + 2 >= src.size() || src[p + 1] != '?')
				return 1;
			const C8* hit = (const C8*)std::memchr(kFrom, src[p + 2], sizeof kFrom - 1);
			if(!hit)
				return 1;
			c = kTo[hit - kFrom];
			return 3;
		}

		U64 newlineLen(const String& s, U64 i) {
			if(i < s.size() && s[i] == '\n')
				return 1;
			return i + 1 < s.size() && s[i] == '\r' && s[i + 1] == '\n' ? 2 : 0;
		}

		U64 bytesEq(U64 w, C8 b) {
			constexpr U64 lo = 0x7F7F7F7F7F7F7F7Full;
			U64 x = w ^ ((U64)(U8)b * kBytes1);
			return ~(((x & lo) + lo) | x | lo);
		}

		// trigraph + splice + newline norm in one pass
		void splice(const String& src, String& out, List<LineMark>& marks) {
			U32 line = 1;
			U64 i = 0, n = src.size();
			out.reserve(n + 1);
			const C8* data = src.data();
			while(i < n) {
				// fast path: bulk-copy a run with no splice/trigraph/CR triggers
				U64 start = i;
				while(i + 8 <= n) {
					U64 w;
					std::memcpy(&w, data + i, 8);
					if(bytesEq(w, '\\') | bytesEq(w, '\r') | bytesEq(w, '?'))
						break;
					line += (U32)(((bytesEq(w, '\n') >> 7) * kBytes1) >> 56);
					i += 8;
				}
				while(i < n) {
					C8 c = data[i];
					if(c == '\\' || c == '\r' || c == '?')
						break;
					if(c == '\n')
						++line;
					++i;
				}
				if(i > start)
					out.append(data + start, i - start);
				if(i >= n)
					break;

				C8 c;
				U64 len = decodeTrigraph(src, i, c);
				// backslash-newline splice (backslash may be a trigraph)
				U64 nl = c == '\\' ? newlineLen(src, i + len) : 0;
				if(nl) {
					i += len + nl;
					++line;
					marks.push_back({(U32)out.size(), line});
					continue;
				}
				if(c == '\r') {
					if(i + 1 < n && src[i + 1] == '\n')
						++i;
					out.push_back('\n');
					++line;
					++i;
					continue;
				}
				out.push_back(c);
				i += len;
			}
			if(out.empty() || out.back() != '\n')
				out.push_back('\n');
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

		// apply splice line-corrections at offsets <= p
		inline void PpLexer::advanceTo(U64 p) {
			while(mi < marks.size() && marks[mi].off <= p) {
				line = marks[mi].line;
				++mi;
			}
		}

		inline void PpLexer::push(Pk kind, U64 start, U64 end) {
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
			PpToken t;
			t.kind = kind;
			t.text = in.intern(sv);
			t.spaceBefore = spacePending;
			t.bol = bolPending;
			t.line = line;
			t.file = file;
			r.toks.push_back(t);
			bolPending = false;
			spacePending = false;
		}

		U64 PpLexer::skipComment(U64 i) {
			U64 n = s.size();
			if(s[i + 1] == '/') {
				while(i < n && s[i] != '\n')
					++i;
				return i;
			}
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
				return n;
			}
			return i + 2;
		}

		U64 PpLexer::wordEnd(U64 j) const {
			for(;;) {
				if(j < s.size() && isIdentCont(s[j]))
					++j;
				else if(U64 u = ucnLen(s, j))
					j += u;
				else
					return j;
			}
		}

		U64 PpLexer::quotedEnd(U64 i) const {
			C8 quote = s[i];
			U64 j = i + 1, n = s.size();
			while(j < n && s[j] != quote) {
				if(s[j] == '\\' && j + 1 < n)
					j += 2;
				else if(s[j] == '\n')
					break;
				else
					++j;
			}
			return j < n && s[j] == quote ? j + 1 : 0;
		}

		U64 PpLexer::numberEnd(U64 j) const {
			U64 n = s.size();
			for(++j; j < n;) {
				C8 d = s[j];
				if((d == 'e' || d == 'E' || d == 'p' || d == 'P') && j + 1 < n &&
					 (s[j + 1] == '+' || s[j + 1] == '-'))
					j += 2;
				else if(isIdentCont(d) || d == '.')
					++j;
				else
					break;
			}
			return j;
		}

		U64 PpLexer::lexToken(U64 i) {
			U64 n = s.size();
			C8 c = s[i];
			// string / char literal, with optional prefix
			U64 pfx = i;
			if(isIdentStart(c) || ucnLen(s, i)) {
				U64 j = wordEnd(i);
				std::string_view word(s.data() + i, j - i);
				B32 isPrefix = (word == "L" || word == "u" || word == "U" || word == "u8");
				if(!isPrefix || j >= n || (s[j] != '"' && s[j] != '\'')) {
					push(Pk::Id, pfx, j);
					return j;
				}
				// fall through into literal lexing starting at the quote
				i = j;
				c = s[i];
			}

			if(c == '"' || c == '\'') {
				U64 j = quotedEnd(i);
				if(!j) {
					r.ok = false;
					r.err = "unterminated literal";
					return n;
				}
				push(c == '"' ? Pk::Str : Pk::Char, pfx, j);
				return j;
			}

			// pp-number
			if(isDigit(c) || (c == '.' && i + 1 < n && isDigit(s[i + 1]))) {
				U64 j = numberEnd(i);
				push(Pk::Num, i, j);
				return j;
			}

			// punctuator
			U64 plen = punctLen(s, i, n);
			push(Pk::Punct, i, i + plen);
			return i + plen;
		}

		LexResult PpLexer::run() {
			U64 i = 0, n = s.size();
			r.toks.reserve(n / 3 + 8);
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
				if(c == '/' && i + 1 < n && (s[i + 1] == '/' || s[i + 1] == '*')) {
					i = skipComment(i);
					spacePending = true;
				} else {
					i = lexToken(i);
				}
				if(!r.ok)
					return std::move(r);
			}

			PpToken eof;
			eof.kind = Pk::Eof;
			eof.text = in.intern(std::string_view());
			eof.bol = true;
			eof.file = file;
			r.toks.push_back(eof);
			return std::move(r);
		}

		LexResult
		lexAll(const String& s, const List<LineMark>& marks, const String* file, Interner& in) {
			PpLexer lx{s, marks, file, in, {}};
			return lx.run();
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
			for(; p < txt.size(); ++p)
				if(txt[p] == 'u' || txt[p] == 'U')
					v.isU = true;
			v.u = acc;
			return v;
		}
	} // namespace detail
} // namespace rat::cc
