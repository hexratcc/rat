#include "lex/token_stream.h"

#include "lex/preprocess_detail.h"

namespace rat::cc {
	// kTokNames doubles as the keyword/punct spelling table
	static_assert((U32)TokKind::KwAlignas + 1 == (U32)TokKind::LParen,
								"keywords and punctuators must be contiguous");

	namespace detail {
		void pushToken(TokenStream& ts, TokKind kind, const String* text, U32 line) {
			Token t;
			t.kind = kind;
			t.offset = (U32)ts.toks.size();
			t.line = line;
			ts.toks.push_back(t);
			ts.texts.push_back(text);
		}

		TokKind tokKindOf(const PpToken& t, const Map<const String*, TokKind>& kw, TokenStream& ts) {
			if(t.kind == Pk::Num || t.kind == Pk::Char || t.kind == Pk::Str) {
				String lerr;
				TokKind k = classifyLiteral(*t.text, lerr);
				if(k == TokKind::Error && ts.errMsg.empty())
					ts.errMsg = lerr;
				return k;
			}
			auto it = kw.find(t.text);
			if(it != kw.end())
				return it->second;
			if(t.kind == Pk::Id)
				return TokKind::Identifier;
			if(ts.errMsg.empty())
				ts.errMsg = "unexpected token '" + *t.text + "'";
			return TokKind::Error;
		}
	} // namespace detail

	B32 preprocessToTokens(const String& path,
												 const String& source,
												 const PpOptions& opts,
												 TokenStream& ts,
												 String& err) {
		detail::Preprocessor pp(opts);
		if(!pp.run(path, source, err))
			return false;

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
		for(const detail::PpToken& t : pp.out)
			if(t.kind != detail::Pk::Eof && t.kind != detail::Pk::Placemarker)
				detail::pushToken(ts, detail::tokKindOf(t, kindOf, ts), t.text, t.line);
		U32 eofLine = ts.toks.empty() ? 1 : ts.toks.back().line;
		detail::pushToken(ts, TokKind::Eof, pp.interner.intern(std::string_view()), eofLine);

		// tokens reference interned spellings; take ownership of the pool
		ts.ownedText = std::move(pp.interner.store);
		return true;
	}
} // namespace rat::cc
