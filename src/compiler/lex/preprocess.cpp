#include <ctime>
#include <fstream>

#include "lex/preprocess_detail.h"
#include "string.h"

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

		B32 Preprocessor::fail(const String& m) {
			if(ok) {
				ok = false;
				err = m;
			}
			return false;
		}

		B32 Preprocessor::readFile(const String& path, String& content) {
			std::ifstream f(path, std::ios::binary);
			return f && readAll(f, content);
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
				if(fileToks.count(p) || readFile(p, content)) {
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
			auto it = fileToks.find(found);
			if(it == fileToks.end())
				it = fileToks.emplace(found, lexFragment(content, intern(found))).first;
			++includeDepth;
			runTokens(found, it->second);
			--includeDepth;
		}

		void Preprocessor::doInclude(PpSpan rest, const String& path, B32 next) {
			B32 angled = false;
			String fname;
			// the operand as written, else macro-expanded
			if(!includeName(rest, fname, angled) && !includeName(expand(rest), fname, angled)) {
				fail("#include expects \"file\" or <file>");
				return;
			}
			String curDir = path.substr(0, path.find_last_of('/') + 1); // npos + 1 wraps to 0, no dir
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
				std::optional<Macro>& saved = macroStack[key].emplace_back();
				if(it != macros.end())
					saved = it->second;
				return;
			}
			auto it = macroStack.find(key);
			if(it == macroStack.end() || it->second.empty())
				return;
			std::optional<Macro> saved = std::move(it->second.back());
			it->second.pop_back();
			if(saved)
				macros[key] = std::move(*saved);
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

		void Preprocessor::applyPragmaOperators(List<PpToken>& toks, const String& path) {
			U64 kept = 0;
			for(U64 i = 0; i < toks.size(); ++i) {
				B32 isPragma = toks[i].kind == Pk::Id && toks[i].text == idPragma && i + 3 < toks.size();
				B32 hasStringArg = isPragma && isPunct(toks[i + 1], "(") && toks[i + 2].kind == Pk::Str;
				if(hasStringArg && isPunct(toks[i + 3], ")")) {
					List<PpToken> body = lexFragment(destringize(*toks[i + 2].text), intern(path));
					doPragma(PpSpan(body), path);
					i += 3; // the loop steps past the last of the four tokens
					continue;
				}
				if(kept != i)
					toks[kept] = toks[i];
				++kept;
			}
			toks.resize(kept);
		}

		void Preprocessor::flush(PpSpan text) {
			if(text.empty())
				return;
			List<PpToken> e = expand(text);
			applyPragmaOperators(e, fileName);
			const String* file = intern(fileName);
			for(PpToken& t : e) {
				// report lines through any #line adjustment
				t.file = file;
				I64 adj = (I64)t.line + lineDelta;
				t.line = adj >= 1 ? (U32)adj : 1;
				out.push_back(std::move(t));
			}
		}

		B32 Preprocessor::condActive(const List<Cond>& stack) {
			return stack.empty() || stack.back().active;
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

		void Preprocessor::runTokens(const String& path, const List<PpToken>& toks) {
			if(!ok)
				return;

			I64 savedDelta = lineDelta;
			String savedFile = fileName;
			lineDelta = 0;
			fileName = path;

			List<Cond> stack;
			U64 textStart = 0;

			U64 i = 0, n = toks.size();
			while(i < n && ok) {
				U64 start = i;
				U64 j = i + 1;
				while(j < n && !toks[j].bol)
					++j;
				i = j;

				const PpToken& first = toks[start];
				if(!isPunct(first, "#") || !first.bol)
					continue;

				if(condActive(stack))
					flush(PpSpan(toks.data() + textStart, toks.data() + start));
				textStart = j;
				if(start + 1 == j) // null directive
					continue;
				// blank lines after the directive still count, a comment closing it does not
				U32 phys = toks[j - 1].line + toks[j - 1].trailingLines + 1;
				doDirective(PpSpan(toks.data() + start + 1, toks.data() + j), phys, path, stack);
			}

			if(ok && !stack.empty())
				fail(path + ": unterminated #if");

			if(condActive(stack))
				flush(PpSpan(toks.data() + textStart, toks.data() + n));

			lineDelta = savedDelta;
			fileName = savedFile;
		}

		void Preprocessor::installBuiltins() {
			C8 date[32] = "\"??? ?? ????\"";
			C8 time[32] = "\"??:??:??\"";
			time_t now = ::time(nullptr);
			if(struct tm* lt = std::localtime(&now)) {
				std::strftime(date, sizeof date, "\"%b %e %Y\"", lt);
				std::strftime(time, sizeof time, "\"%H:%M:%S\"", lt);
			}
			const String* builtin = intern("<builtin>");
			doDefine(lexFragment(String("__DATE__ ") + date, builtin));
			doDefine(lexFragment(String("__TIME__ ") + time, builtin));
			for(const C8* d : kBuiltinDefs)
				doDefine(lexFragment(d, builtin));
		}

		void Preprocessor::applyCommandLine() {
			for(const String& d : opts.defines) {
				U64 eq = d.find('=');
				String left = d.substr(0, eq);
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
				runTokens(path, lexFragment(source, intern(path)));
			if(!ok)
				errOut = err;
			return ok;
		}

		String Preprocessor::serialize() {
			String s;
			for(const PpToken& t : out) {
				if(!s.empty())
					s += t.bol ? '\n' : ' ';
				s += *t.text;
			}
			s += '\n';
			return s;
		}
	} // namespace detail
} // namespace rat::cc
