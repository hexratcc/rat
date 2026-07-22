#include "Lex/PreprocessDetail.h"

namespace rat::cc {
	namespace detail {
		const HideSet* Preprocessor::internHide(List<const String*> names) {
			if(names.empty())
				return nullptr;
			std::sort(names.begin(), names.end());
			names.erase(std::unique(names.begin(), names.end()), names.end());
			String key(reinterpret_cast<const char*>(names.data()),
								 names.size() * sizeof(const String*));
			auto it = hidePool.find(key);
			if(it != hidePool.end())
				return it->second;
			hideStore.push_back(HideSet{std::move(names)});
			const HideSet* h = &hideStore.back();
			hidePool.emplace(std::move(key), h);
			return h;
		}

		B32 Preprocessor::hideHas(const HideSet* h, const String* name) {
			if(!h)
				return false;
			for(const String* n : h->names)
				if(n == name)
					return true;
			return false;
		}

		const HideSet* Preprocessor::hideInsert(const HideSet* h, const String* n) {
			List<const String*> names = h ? h->names : List<const String*>{};
			names.push_back(n);
			return internHide(std::move(names));
		}

		const HideSet* Preprocessor::hideIntersect(const HideSet* a, const HideSet* b) {
			if(!a || !b)
				return nullptr;
			List<const String*> names;
			for(const String* n : a->names) // both sorted by pointer
				if(std::binary_search(b->names.begin(), b->names.end(), n))
					names.push_back(n);
			return internHide(std::move(names));
		}

		const HideSet* Preprocessor::hideUnion(const HideSet* a, const HideSet* b) {
			if(!a)
				return b;
			if(!b)
				return a;
			List<const String*> names = a->names;
			names.insert(names.end(), b->names.begin(), b->names.end());
			return internHide(std::move(names));
		}

		PpToken Preprocessor::makeNum(U64 v) {
			PpToken t;
			t.kind = Pk::Num;
			t.text = intern(std::to_string(v));
			return t;
		}

		PpToken Preprocessor::makePunct(const String& s) {
			PpToken t;
			t.kind = Pk::Punct;
			t.text = intern(s);
			return t;
		}

		List<PpToken> Preprocessor::lexFragment(const String& text, const String* file) {
			String spliced;
			List<LineMark> marks;
			splice(text, spliced, marks);
			LexResult lr = lexAll(spliced, marks, file, interner);
			if(!lr.ok) {
				fail((file ? *file : String("<fragment>")) + ": " + lr.err);
				return {};
			}
			lr.toks.pop_back(); // drop Eof
			return lr.toks;
		}

		void Preprocessor::pasteInto(PpToken& dst, const PpToken& r) {
			if(dst.kind == Pk::Placemarker) {
				B32 sp = dst.spaceBefore;
				dst = r;
				dst.spaceBefore = sp;
				dst.hide = nullptr;
				return;
			}
			if(r.kind == Pk::Placemarker)
				return;
			String s = *dst.text + *r.text;
			dst.kind = classify(s);
			dst.text = intern(s);
			dst.hide = nullptr;
		}

		PpToken Preprocessor::stringize(const List<PpToken>& a, B32 spaceBefore) {
			String s = "\"";
			for(size_t k = 0; k < a.size(); ++k) {
				const PpToken& t = a[k];
				if(k > 0 && t.spaceBefore)
					s += ' ';
				if(t.kind == Pk::Str || t.kind == Pk::Char) {
					for(char c : *t.text) {
						if(c == '"' || c == '\\')
							s += '\\';
						s += c;
					}
				} else {
					s += *t.text;
				}
			}
			s += '"';
			PpToken out;
			out.kind = Pk::Str;
			out.text = intern(s);
			out.spaceBefore = spaceBefore;
			return out;
		}

		void Preprocessor::appendList(List<PpToken>& os, List<PpToken> src, B32 firstSpace) {
			for(size_t k = 0; k < src.size(); ++k) {
				src[k].bol = false;
				if(k == 0)
					src[k].spaceBefore = firstSpace;
				os.push_back(std::move(src[k]));
			}
		}

		List<PpToken> Preprocessor::substitute(const Macro& m,
																					 const List<List<PpToken>>& args,
																					 const HideSet* hs,
																					 const List<const String*>& formals) {
			List<PpToken> os;
			const List<PpToken>& body = m.body;
			auto idxOf = [&](const String* s) -> int {
				for(size_t k = 0; k < formals.size(); ++k)
					if(formals[k] == s)
						return (int)k;
				return -1;
			};
			auto isVa = [&](const String* s) { return m.variadic && s == m.vaName; };

			size_t i = 0;
			while(i < body.size()) {
				const PpToken& T = body[i];
				B32 isHash = isPunct(T, "#");
				B32 isPaste = isPunct(T, "##");

				// # param -> stringize
				if(m.isFunc && isHash && i + 1 < body.size()) {
					int p = idxOf(body[i + 1].text);
					if(p >= 0) {
						os.push_back(stringize(args[p], T.spaceBefore));
						i += 2;
						continue;
					}
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
				t.hide = hideUnion(t.hide, hs);
				res.push_back(std::move(t));
			}
			return res;
		}

		B32
		Preprocessor::gatherArgs(List<PpToken>& work, List<List<PpToken>>& raw, PpToken& rparen) {
			int depth = 1;
			List<PpToken> cur;
			for(;;) {
				if(work.empty() || work.back().kind == Pk::Eof) {
					fail("unterminated macro argument list");
					return false;
				}
				PpToken w = work.back();
				work.pop_back();
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

		B32 Preprocessor::mapArgs(const Macro& m,
															const List<List<PpToken>>& raw,
															List<List<PpToken>>& actuals) {
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

		void Preprocessor::requeueExpansion(List<PpToken>& r,
																				const PpToken& invoker,
																				List<PpToken>& work) {
			if(!r.empty()) {
				r.front().spaceBefore = invoker.spaceBefore;
				r.front().bol = invoker.bol;
			}
			// push first result token last so it is read next
			for(auto rit = r.rbegin(); rit != r.rend(); ++rit)
				work.push_back(*rit);
		}

		List<PpToken> Preprocessor::expand(PpSpan in) {
			List<PpToken> work;
			work.reserve(in.size());
			for(size_t k = in.size(); k > 0; --k)
				work.push_back(in[k - 1]);
			List<PpToken> os;
			while(!work.empty()) {
				PpToken t = work.back();
				work.pop_back();
				if(t.kind != Pk::Id) {
					os.push_back(t);
					continue;
				}
				auto it = macros.find(t.text);
				if(it == macros.end()) {
					if(t.text == idLine) {
						PpToken n = makeNum((U64)((I64)t.line + lineDelta));
						n.spaceBefore = t.spaceBefore;
						n.bol = t.bol;
						os.push_back(n);
						continue;
					}
					if(t.text == idFile) {
						PpToken n;
						n.kind = Pk::Str;
						n.text = intern("\"" + (fileName.empty() ? (t.file ? *t.file : String()) : fileName) + "\"");
						n.spaceBefore = t.spaceBefore;
						n.bol = t.bol;
						os.push_back(n);
						continue;
					}
					os.push_back(t);
					continue;
				}
				if(hideHas(t.hide, t.text)) {
					os.push_back(t);
					continue;
				}
				const Macro& m = it->second;
				if(!m.isFunc) {
					const HideSet* hs = hideInsert(t.hide, t.text);
					List<PpToken> r = substitute(m, {}, hs, {});
					requeueExpansion(r, t, work);
					continue;
				}
				// function-like: requires a paren next
				if(work.empty() || !isPunct(work.back(), "(")) {
					os.push_back(t);
					continue;
				}
				work.pop_back(); // (
				List<List<PpToken>> raw;
				PpToken rparen;
				if(!gatherArgs(work, raw, rparen))
					return os;
				List<List<PpToken>> actuals;
				if(!mapArgs(m, raw, actuals))
					return os;
				List<const String*> formals = m.params;
				if(m.variadic)
					formals.push_back(m.vaName);
				const HideSet* hs = hideInsert(hideIntersect(t.hide, rparen.hide), t.text);
				List<PpToken> r = substitute(m, actuals, hs, formals);
				requeueExpansion(r, t, work);
			}
			return os;
		}

		void Preprocessor::doDefine(PpSpan toks) {
			if(toks.empty() || toks[0].kind != Pk::Id) {
				fail("#define expects a macro name");
				return;
			}
			Macro m;
			const String* name = toks[0].text;
			if(name == idDefined) {
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
						m.vaName = idVaArgs;
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
						const String* nm = m.body[k + 1].text;
						if(m.variadic && nm == m.vaName)
							okOperand = true;
						for(const String* p : m.params)
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

		void Preprocessor::defineSimple(const String& name, const String& value) {
			Macro m;
			m.body = lexFragment(value, intern("<builtin>"));
			for(PpToken& t : m.body)
				t.bol = false;
			if(!m.body.empty())
				m.body.front().spaceBefore = false;
			macros[intern(name)] = std::move(m);
		}
	} // namespace detail
} // namespace rat::cc
