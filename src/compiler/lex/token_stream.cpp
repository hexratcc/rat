#include "lex/token_stream.h"

#include "lex/preprocess_detail.h"

namespace rat::cc {
	namespace detail {
		// kTokNames doubles as the keyword/punct spelling table
		static_assert((U32)TokKind::KwAlignas + 1 == (U32)TokKind::LParen,
									"keywords and punctuators must be contiguous");

		// classify a pp-number/literal by lexing its text (suffix validation)
		TokKind classifySingle(const String& text, String& err) {
			Lexer lx(text.data(), (U32)text.size());
			Token t = lx.next();
			if(t.kind == TokKind::Error) {
				err = lx.error();
				return TokKind::Error;
			}
			if((U64)t.length != text.size()) {
				err = "malformed token '" + text + "'";
				return TokKind::Error;
			}
			return t.kind;
		}
	} // namespace detail
	using namespace detail;

	B32 preprocessToTokens(const String& path,
												 const String& source,
												 const PpOptions& opts,
												 TokenStream& ts,
												 String& err) {
		detail::Preprocessor pp(opts);
		pp.installBuiltins();
		pp.applyCommandLine();
		if(pp.ok)
			pp.runFile(path, source);
		if(!pp.ok) {
			err = pp.err;
			return false;
		}

		// pointer-keyed keyword/punct maps over the pp interner
		Map<const String*, TokKind> kindOf;
		kindOf.reserve(256);
		for(U32 k = (U32)TokKind::KwAuto; k <= (U32)TokKind::ShrEq; ++k)
			kindOf[pp.interner.intern(tokKindName((TokKind)k))] = (TokKind)k;
		kindOf[pp.interner.intern("__typeof")] = TokKind::KwTypeof;
		kindOf[pp.interner.intern("__typeof__")] = TokKind::KwTypeof;

		ts.fileName = path;
		ts.toks.reserve(pp.out.size() + 1);
		ts.texts.reserve(pp.out.size() + 1);

		const String* emptyText = pp.interner.intern(std::string_view());
		B32 sawError = false;

		auto push = [&](TokKind kind, const String* text, U32 line) {
			Token t;
			t.kind = kind;
			t.offset = (U32)ts.toks.size();
			t.line = line;
			ts.toks.push_back(t);
			ts.texts.push_back(text);
		};

		for(const detail::PpToken& t : pp.out) {
			switch(t.kind) {
			case Pk::Id: {
				auto it = kindOf.find(t.text);
				push(it != kindOf.end() ? it->second : TokKind::Identifier, t.text, t.line);
				break;
			}
			case Pk::Punct: {
				auto it = kindOf.find(t.text);
				if(it != kindOf.end()) {
					push(it->second, t.text, t.line);
				} else {
					if(!sawError) {
						sawError = true;
						ts.errMsg = "unexpected token '" + *t.text + "'";
					}
					push(TokKind::Error, t.text, t.line);
				}
				break;
			}
			case Pk::Num:
			case Pk::Char:
			case Pk::Str: {
				String lerr;
				TokKind k = detail::classifySingle(*t.text, lerr);
				if(k == TokKind::Error && !sawError) {
					sawError = true;
					ts.errMsg = lerr;
				}
				push(k, t.text, t.line);
				break;
			}
			default:
				break;
			}
		}
		push(TokKind::Eof, emptyText, ts.toks.empty() ? 1 : ts.toks.back().line);

		// tokens reference interned spellings; take ownership of the pool
		ts.ownedText = std::move(pp.interner.store);
		return true;
	}
} // namespace rat::cc
