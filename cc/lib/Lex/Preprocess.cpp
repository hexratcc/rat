#include "Lex/Preprocess.h"

#include <ctime>
#include <fstream>

#include "Lex/PreprocessDetail.h"

namespace rat::cc {
	namespace detail {
		void Preprocessor::fail(const String& m) {
			if(ok) {
				ok = false;
				err = m;
			}
		}

		String Preprocessor::dirOf(const String& path) {
			size_t s = path.find_last_of('/');
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

		void Preprocessor::doInclude(PpSpan restIn, const String& curDir, B32 next) {
			B32 angled = false;
			String fname;

			auto reconstruct = [&](PpSpan toks) -> B32 {
				if(toks.empty())
					return false;
				if(toks[0].kind == Pk::Str) {
					fname = unquote(*toks[0].text);
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
						fname += *toks[k].text;
					}
					return false; // missing >
				}
				return false;
			};

		List<PpToken> expanded;
			if(!reconstruct(restIn)) {
				// try macro expansion of the operand
				expanded = expand(restIn);
				if(!reconstruct(PpSpan(expanded))) {
					fail("#include expects \"file\" or <file>");
					return;
				}
			}

			size_t startDir = 0;
			if(next) {
				String cur = curDir;
				stripTrailingSlash(cur);
				for(size_t k = 0; k < opts.includeDirs.size(); ++k) {
					String d = opts.includeDirs[k];
					stripTrailingSlash(d);
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
			if(includeDepth > kMaxIncludeDepth) {
				fail("#include nesting too deep");
				return;
			}
			++includeDepth;
			runFile(found, content);
			--includeDepth;
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

		void Preprocessor::doPragma(PpSpan rest, const String& path) {
			if(rest.size() == 1 && rest[0].kind == Pk::Id && *rest[0].text == "once") {
				pragmaOnce.insert(path);
			} else if(rest.size() >= 1 && rest[0].kind == Pk::Id &&
								(*rest[0].text == "push_macro" || *rest[0].text == "pop_macro")) {
				String mname;
				for(size_t k = 1; k < rest.size(); ++k) {
					if(rest[k].kind == Pk::Str) {
						mname = unquote(*rest[k].text);
						break;
					}
				}
				if(!mname.empty()) {
					const String* key = intern(mname);
					if(*rest[0].text == "push_macro") {
						auto it = macros.find(key);
						SavedMacro sv;
						sv.defined = it != macros.end();
						if(sv.defined)
							sv.macro = it->second;
						macroStack[key].push_back(std::move(sv));
					} else {
						auto it = macroStack.find(key);
						if(it != macroStack.end() && !it->second.empty()) {
							SavedMacro sv = std::move(it->second.back());
							it->second.pop_back();
							if(sv.defined)
								macros[key] = std::move(sv.macro);
							else
								macros.erase(key);
						}
					}
				}
			}
		}

		String Preprocessor::destringize(const String& lit) {
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

		List<PpToken> Preprocessor::applyPragmaOperators(List<PpToken>& toks, const String& path) {
			List<PpToken> out;
			for(size_t i = 0; i < toks.size();) {
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
			for(PpToken& t : e)
				out.push_back(std::move(t));
			textBuf.clear();
		}

		B32 Preprocessor::condActive(const List<Cond>& stack) {
			return stack.empty() ? true : stack.back().active;
		}

		B32 Preprocessor::handleConditional(const String& name, PpSpan rest, List<Cond>& stack) {
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
				B32 active = parent && cond;
				stack.push_back({parent, active, active});
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
						U32 phys = j < n ? toks[j].line : dir.line + 1;
						doLine(PpSpan(toks.data() + d, toks.data() + j), phys);
					}
					continue;
				}

				if(dir.kind != Pk::Id) {
					if(condActive(stack))
						fail("invalid preprocessing directive");
					continue;
				}

				const String& name = *dir.text;
				PpSpan rest(toks.data() + d + 1, toks.data() + j);

				if(handleConditional(name, rest, stack))
					continue;

				if(!condActive(stack))
					continue;

				if(name == "define") {
					doDefine(rest);
				} else if(name == "undef") {
					if(rest.empty() || rest[0].kind != Pk::Id)
						fail("#undef expects an identifier");
					else if(rest[0].text == idDefined)
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
						msg += *rest[k].text;
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

		void Preprocessor::installBuiltins() {
			struct Stamp {
				String date = "\"??? ?? ????\"";
				String time = "\"??:??:??\"";
			};
			static const Stamp stamp = [] {
				Stamp s;
				time_t now = ::time(nullptr);
				struct tm* lt = std::localtime(&now);
				if(lt) {
					char buf[64];
					std::strftime(buf, sizeof buf, "%b %e %Y", lt);
					s.date = String("\"") + buf + "\"";
					std::strftime(buf, sizeof buf, "%H:%M:%S", lt);
					s.time = String("\"") + buf + "\"";
				}
				return s;
			}();
			defineSimple("__DATE__", stamp.date);
			defineSimple("__TIME__", stamp.time);
			defineSimple("__STDC__", "1");
			defineSimple("__STDC_HOSTED__", "1");
			defineSimple("__STDC_VERSION__", "199901L");

			// GNU C extensions
			auto defineFrag = [&](const char* text) { doDefine(lexFragment(text, intern("<builtin>"))); };
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

		void Preprocessor::applyCommandLine() {
			for(const String& d : opts.defines) {
				size_t eq = d.find('=');
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

		String Preprocessor::serialize() {
			String s;
			size_t total = 0;
			for(const PpToken& t : out)
				if(t.kind != Pk::Placemarker && t.kind != Pk::Eof)
					total += t.text->size() + 1;
			s.reserve(total + 2);
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
				s += *t.text;
				first = false;
			}
			s += '\n';
			return s;
		}
	} // namespace detail

	B32 preprocess(
			const String& path, const String& source, const PpOptions& opts, String& out, String& err) {
		detail::Preprocessor pp(opts);
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
