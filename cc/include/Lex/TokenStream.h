#ifndef RAT_CC_TOKENSTREAM_H
#define RAT_CC_TOKENSTREAM_H

#include <deque>

#include "Lex/Lexer.h"
#include "Lex/Preprocess.h"

namespace rat::cc {
	// parser tokens straight from the pp, no serialize-then-relex; drop-in for
	// Lexer: Token.offset indexes the stream, text() gives the interned spelling
	struct TokenStream {
		List<Token> toks;					// always ends with Eof
		List<const String*> texts; // parallel to toks
		std::deque<String> ownedText; // storage backing texts
		String fileName;
		String errMsg; // first conversion error
		size_t pos = 0;

		Token next() {
			Token t = toks[pos];
			if(pos + 1 < toks.size())
				++pos;
			return t;
		}
		const Token& peek() { return toks[pos]; }
		const Token& peek2() { return toks[pos + 1 < toks.size() ? pos + 1 : pos]; }

		String text(const Token& tok) const { return *texts[tok.offset]; }
		const String& file() const { return fileName; }
		const String& error() const { return errMsg; }
		void reset() { pos = 0; }
	};

	// preprocess source and convert straight to parser tokens
	B32 preprocessToTokens(const String& path,
												 const String& source,
												 const PpOptions& opts,
												 TokenStream& ts,
												 String& err);
} // namespace rat::cc

#endif
