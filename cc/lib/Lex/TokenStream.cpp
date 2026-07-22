#include "Lex/TokenStream.h"

#include "Lex/PreprocessDetail.h"

namespace rat::cc {
	namespace {
		using namespace detail;

		// keyword spellings, aligned with TokKind::KwAuto..KwTypeof
		const char* const kKeywords[] = {
				"auto",			"break",		"case",			"char",		 "const",		 "continue", "default",
				"do",				"double",		"else",			"enum",		 "extern",	 "float",		 "for",
				"goto",			"if",				"inline",		"int",		 "long",		 "register", "restrict",
				"return",		"short",		"signed",		"sizeof",	 "static",	 "struct",	 "switch",
				"typedef",	"union",		"unsigned", "void",		 "volatile", "while",		 "_Bool",
				"_Complex", "_Imaginary", "_Generic", "_Static_assert", "__real__", "__imag__",
				"typeof",
		};
		static_assert(sizeof(kKeywords) / sizeof(kKeywords[0]) ==
											(U32)TokKind::KwTypeof - (U32)TokKind::KwAuto + 1,
									"kKeywords must cover every keyword TokKind");

		// punctuator spellings for TokKind::LParen..ShrEq
		struct PunctSpelling {
			const char* s;
			TokKind kind;
		};
		const PunctSpelling kPunctKinds[] = {
				{"(", TokKind::LParen},		{")", TokKind::RParen},		{"{", TokKind::LBrace},
				{"}", TokKind::RBrace},		{"[", TokKind::LBracket}, {"]", TokKind::RBracket},
				{";", TokKind::Semicolon}, {",", TokKind::Comma},		{".", TokKind::Dot},
				{"->", TokKind::Arrow},		{"...", TokKind::Ellipsis}, {"+", TokKind::Plus},
				{"-", TokKind::Minus},		{"*", TokKind::Star},			{"/", TokKind::Slash},
				{"%", TokKind::Percent},	{"++", TokKind::PlusPlus}, {"--", TokKind::MinusMinus},
				{"&", TokKind::Amp},			{"|", TokKind::Pipe},			{"^", TokKind::Caret},
				{"~", TokKind::Tilde},		{"!", TokKind::Bang},			{"&&", TokKind::AmpAmp},
				{"||", TokKind::PipePipe}, {"<", TokKind::Lt},			{">", TokKind::Gt},
				{"<=", TokKind::Le},			{">=", TokKind::Ge},			{"==", TokKind::EqEq},
				{"!=", TokKind::BangEq},	{"<<", TokKind::Shl},			{">>", TokKind::Shr},
				{"?", TokKind::Question}, {":", TokKind::Colon},		{"=", TokKind::Assign},
				{"+=", TokKind::PlusEq},	{"-=", TokKind::MinusEq}, {"*=", TokKind::StarEq},
				{"/=", TokKind::SlashEq}, {"%=", TokKind::PercentEq}, {"&=", TokKind::AmpEq},
				{"|=", TokKind::PipeEq},	{"^=", TokKind::CaretEq}, {"<<=", TokKind::ShlEq},
				{">>=", TokKind::ShrEq},
		};

		// classify a pp-number/literal by lexing its text (suffix validation)
		TokKind classifySingle(const String& text, String& err) {
			Lexer lx(text.data(), (U32)text.size());
			Token t = lx.next();
			if(t.kind == TokKind::Error) {
				err = lx.error();
				return TokKind::Error;
			}
			if((size_t)t.length != text.size()) {
				err = "malformed token '" + text + "'";
				return TokKind::Error;
			}
			return t.kind;
		}
	} // namespace

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
		for(U32 k = 0; k < sizeof(kKeywords) / sizeof(kKeywords[0]); ++k)
			kindOf[pp.interner.intern(kKeywords[k])] = (TokKind)((U32)TokKind::KwAuto + k);
		kindOf[pp.interner.intern("__typeof")] = TokKind::KwTypeof;
		kindOf[pp.interner.intern("__typeof__")] = TokKind::KwTypeof;
		for(const PunctSpelling& p : kPunctKinds)
			kindOf[pp.interner.intern(p.s)] = p.kind;

		ts.fileName = path;
		ts.toks.reserve(pp.out.size() + 1);
		ts.texts.reserve(pp.out.size() + 1);

		const String* emptyText = pp.interner.intern(std::string_view());
		B32 sawError = false;

		auto push = [&](TokKind kind, const String* text, U32 line) {
			Token t;
			t.kind = kind;
			t.offset = (U32)ts.toks.size();
			t.length = (U32)text->size();
			t.line = line;
			t.col = 1;
			ts.toks.push_back(t);
			ts.texts.push_back(text);
		};

		for(const detail::PpToken& t : pp.out) {
			if(t.kind == Pk::Placemarker || t.kind == Pk::Eof)
				continue;
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
				TokKind k = classifySingle(*t.text, lerr);
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
