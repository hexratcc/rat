#include "lex/preprocess_detail.h"

#include "hash.h"

namespace rat::cc {
	namespace detail {
		// names arrive sorted+unique (kept by every ctor below), so interning
		// is a pointer-hash lookup with no sort
		const HideSet* Preprocessor::internHide(List<const String*> names) {
			if(names.empty())
				return nullptr;
			U64 h = kFnvBasis;
			for(const String* p : names)
				hashMix(h, (U64)(uintptr_t)p);
			List<const HideSet*>& bucket = hidePool[h];
			for(const HideSet* c : bucket)
				if(c->names == names)
					return c;
			hideStore.push_back(HideSet{std::move(names)});
			const HideSet* hp = &hideStore.back();
			bucket.push_back(hp);
			return hp;
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
			if(!h)
				return internHide({n});
			const List<const String*>& hn = h->names;
			auto pos = std::lower_bound(hn.begin(), hn.end(), n);
			if(pos != hn.end() && *pos == n)
				return h; // already present
			List<const String*> names;
			names.reserve(hn.size() + 1);
			names.insert(names.end(), hn.begin(), pos);
			names.push_back(n);
			names.insert(names.end(), pos, hn.end());
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
			if(!a || a == b)
				return b;
			if(!b)
				return a;
			const List<const String*>& an = a->names;
			const List<const String*>& bn = b->names;
			List<const String*> names;
			names.reserve(an.size() + bn.size());
			std::set_union(an.begin(), an.end(), bn.begin(), bn.end(), std::back_inserter(names));
			if(names.size() == an.size())
				return a; // b is a subset of a
			if(names.size() == bn.size())
				return b; // a is a subset of b
			return internHide(std::move(names));
		}

		PpToken Preprocessor::makeToken(Pk kind, const String& text) {
			PpToken t;
			t.kind = kind;
			t.text = intern(text);
			return t;
		}

		PpToken Preprocessor::makeTokenAt(Pk kind, const String& text, const PpToken& at) {
			PpToken t = makeToken(kind, text);
			t.line = at.line;
			t.col = at.col;
			return t;
		}

		List<PpToken> Preprocessor::lexFragment(const String& text, const String* file) {
			String spliced;
			List<LineMark> marks;
			splice(text, spliced, marks);
			PpLexer lx{spliced, marks, file, interner, {}, {}};
			lx.run();
			if(!lx.err.empty()) {
				fail((file ? *file : String("<fragment>")) + ": " + lx.err);
				return {};
			}
			lx.toks.pop_back(); // drop Eof
			return std::move(lx.toks);
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
			for(U64 k = 0; k < a.size(); ++k) {
				const PpToken& t = a[k];
				if(k > 0 && t.spaceBefore)
					s += ' ';
				if(t.kind == Pk::Str || t.kind == Pk::Char) {
					for(C8 c : *t.text) {
						if(c == '"' || c == '\\')
							s += '\\';
						s += c;
					}
				} else {
					s += *t.text;
				}
			}
			s += '"';
			PpToken out = makeToken(Pk::Str, s);
			out.spaceBefore = spaceBefore;
			return out;
		}

		void Preprocessor::appendList(List<PpToken>& os, List<PpToken> src, B32 firstSpace) {
			for(U64 k = 0; k < src.size(); ++k) {
				src[k].bol = false;
				if(k == 0)
					src[k].spaceBefore = firstSpace;
				os.push_back(std::move(src[k]));
			}
		}

		I32 formalIndex(const Macro& m, const String* s) {
			for(U64 k = 0; k < m.formals.size(); ++k)
				if(m.formals[k] == s)
					return (I32)k;
			return -1;
		}

		void Preprocessor::pasteArg(const List<PpToken>& a, B32 commaVa, List<PpToken>& os) {
			if(commaVa) {
				if(a.empty())
					os.pop_back(); // drop the comma
				else
					appendList(os, a, true); // keep comma, no paste
				return;
			}
			if(a.empty())
				return; // paste with empty operand -> previous unchanged
			pasteInto(os.back(), a.front());
			os.insert(os.end(), a.begin() + 1, a.end());
		}

		List<PpToken>
		Preprocessor::substitute(const Macro& m, const ArgLists& args, const HideSet* hs) {
			List<PpToken> os;
			const List<PpToken>& body = m.body;
			U64 i = 0;
			while(i < body.size()) {
				const PpToken& T = body[i];
				B32 isHash = isPunct(T, "#");
				B32 isPaste = isPunct(T, "##");

				// # param -> stringize
				if(m.isFunc && isHash && i + 1 < body.size()) {
					I32 p = formalIndex(m, body[i + 1].text);
					if(p >= 0) {
						os.push_back(stringize(args[p], T.spaceBefore));
						i += 2;
						continue;
					}
				}

				// ## token -> paste onto the previous token
				if(isPaste && !os.empty() && i + 1 < body.size()) {
					const PpToken& R = body[i + 1];
					I32 p = formalIndex(m, R.text);
					if(p >= 0) {
						// GNU comma elision
						B32 commaVa = m.vaName && R.text == m.vaName && isPunct(os.back(), ",");
						pasteArg(args[p], commaVa, os);
					} else {
						pasteInto(os.back(), R);
					}
					i += 2;
					continue;
				}

				I32 p = formalIndex(m, T.text);
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

			return applyHideSet(os, hs);
		}

		// drop placemarkers, union hide set into every token, memoize on the
		// incoming set (nearly always repeated)
		List<PpToken> Preprocessor::applyHideSet(List<PpToken>& os, const HideSet* hs) {
			List<PpToken> res;
			res.reserve(os.size());
			const HideSet* memoIn = (const HideSet*)&res; // impossible value
			const HideSet* memoOut = nullptr;
			for(PpToken& t : os) {
				if(t.kind == Pk::Placemarker)
					continue;
				if(t.hide != memoIn) {
					memoIn = t.hide;
					memoOut = hideUnion(t.hide, hs);
				}
				t.hide = memoOut;
				res.push_back(std::move(t));
			}
			return res;
		}

		U32 Preprocessor::matchParen(const List<PpToken>& arg, U32 open) {
			I32 depth = 0;
			for(U32 i = open; i < arg.size(); ++i) {
				if(isPunct(arg[i], "("))
					++depth;
				else if(isPunct(arg[i], ")") && --depth == 0)
					return i;
			}
			return (U32)arg.size();
		}

		B32 Preprocessor::gatherArgs(List<PpToken>& work, ArgLists& raw, PpToken& rparen) {
			I32 depth = 1;
			List<PpToken> cur;
			for(;;) {
				if(work.empty())
					return fail("unterminated macro argument list");
				PpToken w = work.back();
				work.pop_back();
				if(isPunct(w, "("))
					++depth;
				else if(isPunct(w, ")"))
					--depth;
				if(depth == 0) {
					rparen = w;
					raw.push_back(std::move(cur));
					return true;
				}
				if(depth == 1 && isPunct(w, ",")) {
					raw.push_back(std::move(cur));
					cur.clear();
				} else {
					cur.push_back(w);
				}
			}
		}

		B32 Preprocessor::mapArgs(const Macro& m, const ArgLists& raw, ArgLists& actuals) {
			if(!m.vaName) {
				if(m.formals.empty() && raw.size() == 1 && raw[0].empty())
					return true;
				if(raw.size() != m.formals.size())
					return fail("macro invoked with wrong number of arguments");
				actuals = raw;
				return true;
			}
			U64 np = m.formals.size() - 1; // the named ones
			if(raw.size() < np)
				return fail("macro invoked with too few arguments");
			actuals.assign(raw.begin(), raw.begin() + np);
			List<PpToken> va;
			for(U64 j = np; j < raw.size(); ++j) {
				if(j > np)
					va.push_back(makeToken(Pk::Punct, ","));
				va.insert(va.end(), raw[j].begin(), raw[j].end());
			}
			actuals.push_back(std::move(va));
			return true;
		}

		AttrArgs Preprocessor::scanAttrArgs(const ArgLists& raw) {
			AttrArgs attr;
			for(const List<PpToken>& arg : raw) {
				for(U32 i = 0; i < arg.size(); ++i) {
					const PpToken& w = arg[i];
					if(w.kind != Pk::Id)
						continue;
					if(w.text == idNoinline || w.text == idNoinline2)
						attr.noinline = true;
					B32 isAlias = (w.text == idAlias || w.text == idAlias2) && i + 2 < arg.size();
					if(isAlias && isPunct(arg[i + 1], "(") && arg[i + 2].kind == Pk::Str)
						attr.aliasTarget = &arg[i + 2];
					B32 isAligned = (w.text == idAligned || w.text == idAligned2) && i + 1 < arg.size();
					if(!isAligned || !isPunct(arg[i + 1], "("))
						continue;
					U32 e = matchParen(arg, i + 1);
					if(e < arg.size())
						attr.alignParens = PpSpan(&arg[i + 1], &arg[e] + 1);
				}
			}
			return attr;
		}

		void Preprocessor::emitAttrMarkers(const PpToken& at, const AttrArgs& attr, List<PpToken>& os) {
			if(attr.noinline) {
				PpToken n = at;
				n.text = idNoinlineMark;
				os.push_back(n);
			}
			if(attr.aliasTarget) {
				PpToken n = at;
				n.text = idAliasMark;
				os.push_back(n);
				os.push_back(makeTokenAt(Pk::Punct, "(", at));
				os.push_back(*attr.aliasTarget);
				os.push_back(makeTokenAt(Pk::Punct, ")", at));
			}
		}

		void
		Preprocessor::requeueExpansion(List<PpToken>& r, const PpToken& invoker, List<PpToken>& work) {
			if(!r.empty()) {
				r.front().spaceBefore = invoker.spaceBefore;
				r.front().bol = invoker.bol;
			}
			// expanded tokens report the invocation site
			for(PpToken& t : r) {
				t.line = invoker.line;
				t.col = invoker.col;
			}
			// push first result token last so it is read next
			work.insert(work.end(), r.rbegin(), r.rend());
		}

		B32 Preprocessor::expandBuiltinName(const PpToken& t, List<PpToken>& os) {
			PpToken n;
			if(t.text == idLine) {
				n = makeTokenAt(Pk::Num, std::to_string((U64)((I64)t.line + lineDelta)), t);
			} else if(t.text == idFile) {
				String file = fileName;
				if(file.empty() && t.file)
					file = *t.file;
				n = makeTokenAt(Pk::Str, "\"" + file + "\"", t);
			} else {
				return false;
			}
			n.spaceBefore = t.spaceBefore;
			n.bol = t.bol;
			os.push_back(n);
			return true;
		}

		void Preprocessor::requeueAlignas(const PpToken& at, PpSpan alignParens, List<PpToken>& work) {
			if(alignParens.empty())
				return;
			List<PpToken> r;
			PpToken n = at;
			n.text = idAlignas;
			r.push_back(n);
			r.insert(r.end(), alignParens.b, alignParens.e);
			requeueExpansion(r, at, work);
		}

		List<PpToken> Preprocessor::expand(PpSpan in) {
			List<PpToken> work(std::make_reverse_iterator(in.e), std::make_reverse_iterator(in.b));
			List<PpToken> os;
			os.reserve(in.size());
			while(!work.empty()) {
				PpToken t = work.back();
				work.pop_back();
				if(t.kind != Pk::Id) {
					os.push_back(t);
					continue;
				}
				auto it = macros.find(t.text);
				if(it == macros.end()) {
					if(!expandBuiltinName(t, os))
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
					List<PpToken> r = substitute(m, {}, hs);
					requeueExpansion(r, t, work);
					continue;
				}
				// function-like: requires a paren next
				if(work.empty() || !isPunct(work.back(), "(")) {
					os.push_back(t);
					continue;
				}
				work.pop_back(); // (
				ArgLists raw;
				PpToken rparen;
				if(!gatherArgs(work, raw, rparen))
					return os;
				ArgLists actuals;
				if(!mapArgs(m, raw, actuals))
					return os;
				// the builtin __attribute__ erases its list, but noinline and alias survive
				// as marker keywords and aligned(n) as the equivalent _Alignas(n), so the
				// parser can honor all three
				if(t.text == idAttr || t.text == idAttr2) {
					AttrArgs attr = scanAttrArgs(raw);
					emitAttrMarkers(t, attr, os);
					requeueAlignas(t, attr.alignParens, work);
					continue;
				}
				const HideSet* hs = hideInsert(hideIntersect(t.hide, rparen.hide), t.text);
				List<PpToken> r = substitute(m, actuals, hs);
				requeueExpansion(r, t, work);
			}
			return os;
		}

		B32 Preprocessor::parseMacroParams(PpSpan toks, U64& i, Macro& m) {
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
					m.vaName = idVaArgs;
					++i;
					break;
				}
				if(t.kind == Pk::Id && expectName) {
					// GNU named variadic
					if(i + 1 < toks.size() && isPunct(toks[i + 1], "...")) {
						m.vaName = t.text;
						i += 2;
						break;
					}
					m.formals.push_back(t.text);
					expectName = false;
					++i;
					continue;
				}
				return fail("malformed macro parameter list");
			}
			if(i >= toks.size() || !isPunct(toks[i], ")"))
				return fail("missing ')' in macro parameter list");
			++i; // )
			if(m.vaName)
				m.formals.push_back(m.vaName);
			return true;
		}

		B32 Preprocessor::checkMacroBody(const Macro& m) {
			if(!m.body.empty() && (isPunct(m.body.front(), "##") || isPunct(m.body.back(), "##")))
				return fail("'##' cannot appear at either end of a macro expansion");
			if(!m.isFunc)
				return true;
			for(U64 k = 0; k < m.body.size(); ++k) {
				if(!isPunct(m.body[k], "#"))
					continue;
				B32 hasIdOperand = k + 1 < m.body.size() && m.body[k + 1].kind == Pk::Id;
				if(!hasIdOperand || formalIndex(m, m.body[k + 1].text) < 0)
					return fail("'#' is not followed by a macro parameter");
			}
			return true;
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
			U64 i = 1;
			B32 funcLike = i < toks.size() && isPunct(toks[i], "(") && !toks[i].spaceBefore;
			if(funcLike && !parseMacroParams(toks, i, m))
				return;
			for(; i < toks.size(); ++i) {
				PpToken b = toks[i];
				b.bol = false;
				if(m.body.empty())
					b.spaceBefore = false;
				m.body.push_back(b);
			}
			if(checkMacroBody(m))
				macros[name] = std::move(m);
		}
	} // namespace detail
} // namespace rat::cc
