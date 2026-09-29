#include "lex/preprocess.h"

#include <ctime>
#include <fstream>
#include <sstream>

#include "lex/preprocess_detail.h"

namespace rat::cc {
	namespace detail {
		String withSlash(String dir) {
			if(!dir.empty() && dir.back() != '/')
				dir += '/';
			return dir;
		}

		String joinSpelling(PpSpan toks) {
			String s;
			for(U64 k = 0; k < toks.size(); ++k) {
				if(k && toks[k].spaceBefore)
					s += ' ';
				s += *toks[k].text;
			}
			return s;
		}

		List<String> stampDefs() {
			String date = "\"??? ?? ????\"";
			String time = "\"??:??:??\"";
			time_t now = ::time(nullptr);
			if(struct tm* lt = std::localtime(&now)) {
				C8 buf[64];
				std::strftime(buf, sizeof buf, "%b %e %Y", lt);
				date = String("\"") + buf + "\"";
				std::strftime(buf, sizeof buf, "%H:%M:%S", lt);
				time = String("\"") + buf + "\"";
			}
			return {"__DATE__ " + date, "__TIME__ " + time};
		}

		// clang-format off
		const C8* const kBuiltinDefs[] = {
				"__STDC__ 1", "__STDC_HOSTED__ 1", "__STDC_VERSION__ 199901L",
				// GNU C extensions
				"__attribute__(x)", "__attribute(x)", "__asm__ asm", "__asm asm", "__restrict",
				"__restrict__ restrict", "__inline inline", "__inline__ inline", "__volatile__ volatile",
				"__volatile volatile", "__extension__", "__alignof__ _Alignof", "__alignof _Alignof",
				"__signed__ signed", "__signed signed", "__const const", "__thread",
				// GCC extended floating types
				"_Float32 float", "_Float32x double", "_Float64 double", "_Float64x double",
				"_Float128 double", "_Float128x double",
				// GCC 128-bit integers
				"__int128 long long",
		};
		// clang-format on

		void Preprocessor::fail(const String& m) {
			if(ok) {
				ok = false;
				err = m;
			}
		}

		String Preprocessor::dirOf(const String& path) {
			U64 s = path.find_last_of('/');
			return s == String::npos ? String() : path.substr(0, s + 1);
		}

		B32 Preprocessor::readFile(const String& path, String& content) {
			std::ifstream f(path, std::ios::binary);
			if(!f)
				return false;
			std::ostringstream ss;
			ss << f.rdbuf();
			content = ss.str();
			return true;
		}

		B32 Preprocessor::includeName(PpSpan toks, String& fname, B32& angled) {
			if(toks.empty())
				return false;
			if(toks[0].kind == Pk::Str) {
				fname = unquote(*toks[0].text);
				angled = false;
				return true;
			}
			if(!isPunct(toks[0], "<"))
				return false;
			for(U64 k = 1; k < toks.size(); ++k) {
				if(isPunct(toks[k], ">")) {
					fname = joinSpelling(PpSpan(toks.b + 1, toks.b + k));
					angled = true;
					return true;
				}
			}
			return false; // missing >
		}

		U64 Preprocessor::nextIncludeDir(const String& curDir) {
			U64 startDir = 0;
			U64 best = 0;
			for(U64 k = 0; k < opts.includeDirs.size(); ++k) {
				String d = withSlash(opts.includeDirs[k]);
				if(d.size() > best && d.size() <= curDir.size() && curDir.compare(0, d.size(), d) == 0) {
					best = d.size();
					startDir = k + 1;
				}
			}
			return startDir;
		}

		void Preprocessor::enterInclude(const List<String>& tries, const String& fname) {
			String found;
			String content;
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
			if(includeDepth > kMaxIncludeDepth) {
				fail("#include nesting too deep");
				return;
			}
			++includeDepth;
			runFile(found, content);
			--includeDepth;
		}

		void Preprocessor::doInclude(PpSpan rest, const String& path, B32 next) {
			B32 angled = false;
			String fname;
			B32 named = includeName(rest, fname, angled);
			if(!named) // try macro expansion of the operand
				named = includeName(expand(rest), fname, angled);
			if(!named) {
				fail("#include expects \"file\" or <file>");
				return;
			}
			String curDir = dirOf(path);
			List<String> tries;
			if(isAbsPath(fname)) {
				tries.push_back(fname);
			} else {
				if(!next && !angled)
					tries.push_back(curDir + fname);
				for(U64 k = next ? nextIncludeDir(curDir) : 0; k < opts.includeDirs.size(); ++k)
					tries.push_back(withSlash(opts.includeDirs[k]) + fname);
			}
			enterInclude(tries, fname);
		}

		void Preprocessor::doLine(PpSpan restIn, U32 physicalNextLine) {
			List<PpToken> rest = expand(restIn);
			if(rest.empty() || rest[0].kind != Pk::Num) {
				fail("#line expects a line number");
				return;
			}
			U64 n = parseNumLit(*rest[0].text).u;
			lineDelta = (I64)n - (I64)physicalNextLine;
			if(rest.size() > 1 && rest[1].kind == Pk::Str)
				fileName = unquote(*rest[1].text);
		}

		void Preprocessor::pushPopMacro(PpSpan rest, B32 push) {
			String mname;
			for(U64 k = 1; k < rest.size(); ++k) {
				if(rest[k].kind == Pk::Str) {
					mname = unquote(*rest[k].text);
					break;
				}
			}
			if(mname.empty())
				return;
			const String* key = intern(mname);
			if(push) {
				auto it = macros.find(key);
				SavedMacro sv;
				sv.defined = it != macros.end();
				if(sv.defined)
					sv.macro = it->second;
				macroStack[key].push_back(std::move(sv));
				return;
			}
			auto it = macroStack.find(key);
			if(it == macroStack.end() || it->second.empty())
				return;
			SavedMacro sv = std::move(it->second.back());
			it->second.pop_back();
			if(sv.defined)
				macros[key] = std::move(sv.macro);
			else
				macros.erase(key);
		}

		void Preprocessor::doPragma(PpSpan rest, const String& path) {
			if(rest.empty() || rest[0].kind != Pk::Id)
				return;
			const String& what = *rest[0].text;
			if(what == "once" && rest.size() == 1)
				pragmaOnce.insert(path);
			else if(what == "push_macro" || what == "pop_macro")
				pushPopMacro(rest, what == "push_macro");
		}

		String Preprocessor::destringize(const String& lit) {
			U64 b = 0, e = lit.size();
			while(b < e && lit[b] != '"')
				++b; // skip optional L prefix
			if(b < e && lit[b] == '"')
				++b;
			if(e > b && lit[e - 1] == '"')
				--e;
			String out;
			for(U64 i = b; i < e; ++i) {
				if(lit[i] == '\\' && i + 1 < e && (lit[i + 1] == '"' || lit[i + 1] == '\\'))
					++i;
				out += lit[i];
			}
			return out;
		}

		List<PpToken> Preprocessor::applyPragmaOperators(List<PpToken>& toks, const String& path) {
			B32 any = false;
			for(const PpToken& t : toks)
				if(t.kind == Pk::Id && *t.text == "_Pragma") {
					any = true;
					break;
				}
			if(!any)
				return std::move(toks);
			List<PpToken> out;
			for(U64 i = 0; i < toks.size();) {
				if(toks[i].kind == Pk::Id && *toks[i].text == "_Pragma" && i + 3 < toks.size() &&
					 isPunct(toks[i + 1], "(") && toks[i + 2].kind == Pk::Str && isPunct(toks[i + 3], ")")) {
					List<PpToken> body = lexFragment(destringize(*toks[i + 2].text), intern(path));
					doPragma(PpSpan(body), path);
					i += 4;
					continue;
				}
				out.push_back(std::move(toks[i]));
				++i;
			}
			return out;
		}

		void Preprocessor::flush(List<PpToken>& textBuf) {
			if(textBuf.empty())
				return;
			List<PpToken> e = expand(PpSpan(textBuf));
			e = applyPragmaOperators(e, fileName);
			for(PpToken& t : e) {
				// report lines through any #line adjustment
				I64 adj = (I64)t.line + lineDelta;
				t.line = adj >= 1 ? (U32)adj : 1;
				out.push_back(std::move(t));
			}
			textBuf.clear();
		}

		B32 Preprocessor::condActive(const List<Cond>& stack) {
			return stack.empty() ? true : stack.back().active;
		}

		void Preprocessor::pushCond(const String& name, PpSpan rest, List<Cond>& stack) {
			B32 parent = condActive(stack);
			B32 cond = false;
			if(parent && name == "if")
				cond = evalExpr(rest);
			else if(parent && (rest.empty() || rest[0].kind != Pk::Id))
				fail("#" + name + " expects an identifier");
			else if(parent)
				cond = isDefined(rest[0].text) == (name == "ifdef");
			B32 active = parent && cond;
			stack.push_back({parent, active, active});
		}

		B32 Preprocessor::handleConditional(const String& name, PpSpan rest, List<Cond>& stack) {
			if(name == "if" || name == "ifdef" || name == "ifndef") {
				pushCond(name, rest, stack);
				return true;
			}
			if(name != "elif" && name != "else" && name != "endif")
				return false;
			if(stack.empty()) {
				fail("#" + name + " without #if");
				return true;
			}
			Cond& c = stack.back();
			if(name == "endif") {
				stack.pop_back();
			} else if(c.sawElse) {
				fail("#" + name + " after #else");
			} else if(name == "elif") {
				c.active = c.parentActive && !c.taken && evalExpr(rest);
				c.taken |= c.active;
			} else {
				c.sawElse = true;
				c.active = c.parentActive && !c.taken;
				c.taken = true;
			}
			return true;
		}

		void Preprocessor::doDirective(PpSpan line, U32 phys, const String& path, List<Cond>& stack) {
			const PpToken& dir = line[0];
			if(dir.kind == Pk::Num) {
				if(condActive(stack))
					doLine(line, phys);
				return;
			}
			if(dir.kind != Pk::Id) {
				if(condActive(stack))
					fail("invalid preprocessing directive");
				return;
			}
			const String& name = *dir.text;
			PpSpan rest(line.b + 1, line.e);
			if(handleConditional(name, rest, stack) || !condActive(stack))
				return;
			if(name == "define")
				doDefine(rest);
			else if(name == "undef")
				doUndef(rest);
			else if(name == "include" || name == "include_next")
				doInclude(rest, path, name == "include_next");
			else if(name == "error")
				fail(path + ": #error " + joinSpelling(rest));
			else if(name == "pragma")
				doPragma(rest, path);
			else if(name == "line")
				doLine(rest, phys);
			else
				fail("invalid preprocessing directive #" + name);
		}

		void Preprocessor::doUndef(PpSpan rest) {
			if(rest.empty() || rest[0].kind != Pk::Id)
				fail("#undef expects an identifier");
			else if(rest[0].text == idDefined)
				fail("'defined' cannot be used as a macro name");
			else
				macros.erase(rest[0].text);
		}

		void Preprocessor::runFile(const String& path, const String& source) {
			if(!ok)
				return;

			List<PpToken> toks = lexFragment(source, intern(path));
			if(!ok)
				return;

			I64 savedDelta = lineDelta;
			String savedFile = fileName;
			lineDelta = 0;
			fileName = path;

			List<Cond> stack;
			List<PpToken> textBuf;

			U64 i = 0, n = toks.size();
			while(i < n && ok) {
				U64 start = i;
				U64 j = i + 1;
				while(j < n && !toks[j].bol)
					++j;
				i = j;

				const PpToken& first = toks[start];
				if(!isPunct(first, "#") || !first.bol) {
					if(condActive(stack))
						textBuf.insert(textBuf.end(), toks.begin() + start, toks.begin() + j);
					continue;
				}

				flush(textBuf);
				if(start + 1 == j) // null directive
					continue;
				U32 phys = j < n ? toks[j].line : toks[start + 1].line + 1;
				doDirective(PpSpan(toks.data() + start + 1, toks.data() + j), phys, path, stack);
			}

			if(ok && !stack.empty())
				fail(path + ": unterminated #if");

			flush(textBuf);

			lineDelta = savedDelta;
			fileName = savedFile;
		}

		void Preprocessor::defineFragment(const String& text, const C8* file) {
			doDefine(lexFragment(text, intern(file)));
		}

		void Preprocessor::installBuiltins() {
			static const List<String> stamps = stampDefs();
			for(const String& d : stamps)
				defineFragment(d, "<builtin>");
			for(const C8* d : kBuiltinDefs)
				defineFragment(d, "<builtin>");
		}

		void Preprocessor::applyCommandLine() {
			for(const String& d : opts.defines) {
				U64 eq = d.find('=');
				String left = eq == String::npos ? d : d.substr(0, eq);
				String right = eq == String::npos ? String("1") : d.substr(eq + 1);
				List<PpToken> all = lexFragment(left, intern("<command-line>"));
				List<PpToken> rt = lexFragment(right, intern("<command-line>"));
				all.insert(all.end(), rt.begin(), rt.end());
				doDefine(all);
			}
			for(const String& u : opts.undefs)
				macros.erase(intern(u));
		}

		B32 Preprocessor::run(const String& path, const String& source, String& errOut) {
			installBuiltins();
			applyCommandLine();
			if(ok)
				runFile(path, source);
			if(!ok)
				errOut = err;
			return ok;
		}

		String Preprocessor::serialize() {
			String s;
			U64 total = 0;
			for(const PpToken& t : out)
				if(t.kind != Pk::Placemarker && t.kind != Pk::Eof)
					total += t.text->size() + 1;
			s.reserve(total + 2);
			for(const PpToken& t : out) {
				if(t.kind == Pk::Placemarker || t.kind == Pk::Eof)
					continue;
				if(!s.empty())
					s += t.bol ? '\n' : ' ';
				s += *t.text;
			}
			s += '\n';
			return s;
		}
	} // namespace detail

	B32 preprocess(
			const String& path, const String& source, const PpOptions& opts, String& out, String& err) {
		detail::Preprocessor pp(opts);
		if(!pp.run(path, source, err))
			return false;
		out = pp.serialize();
		return true;
	}
} // namespace rat::cc
