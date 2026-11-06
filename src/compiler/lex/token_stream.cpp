#include "lex/token_stream.h"

#include "lex/preprocess_detail.h"

#include <algorithm>

namespace rat::cc {
	// kTokNames doubles as the keyword/punct spelling table
	static_assert((U32)TokKind::KwAlignas + 1 == (U32)TokKind::LParen,
								"keywords and punctuators must be contiguous");

	namespace detail {
		void pushToken(TokenStream& ts, Token tok, const String* text) {
			tok.offset = (U32)ts.toks.size();
			ts.toks.push_back(tok);
			ts.texts.push_back(text);
		}

		U16 fileIndex(TokenStream& ts, const String* file, Map<const String*, U16>& indexOfFile) {
			// shortcut: files past the 65536th share its index
			auto [it, added] = indexOfFile.try_emplace(file, (U16)std::min<U64>(ts.files.size(), 0xFFFF));
			if(added)
				ts.files.push_back(*file);
			return it->second;
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
		Map<const String*, U16> indexOfFile;
		const String* lastFile = nullptr;
		U16 file = 0;
		for(const detail::PpToken& t : pp.out) {
			if(t.kind == detail::Pk::Eof || t.kind == detail::Pk::Placemarker)
				continue;
			if(t.file != lastFile) {
				file = detail::fileIndex(ts, t.file, indexOfFile);
				lastFile = t.file;
			}
			detail::pushToken(ts, {detail::tokKindOf(t, kindOf, ts), 0, t.line, 1, file}, t.text);
		}
		if(ts.files.empty())
			ts.files.push_back(path);
		U32 eofLine = ts.toks.empty() ? 1 : ts.toks.back().line;
		detail::pushToken(
				ts, {TokKind::Eof, 0, eofLine, 1, file}, pp.interner.intern(std::string_view()));

		// tokens reference interned spellings; take ownership of the pool
		ts.ownedText = std::move(pp.interner.store);
		return true;
	}
} // namespace rat::cc
