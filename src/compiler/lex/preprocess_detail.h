#ifndef RAT_CC_PREPROCESS_DETAIL_H
#define RAT_CC_PREPROCESS_DETAIL_H

#include <deque>
#include <string_view>

#include "lex/preprocess.h"

namespace rat::cc {
	namespace detail {
		enum class Pk : U8 {
			Eof,
			Id,					// identifier/kw
			Num,				// pp-number
			Char,				// character constant
			Str,				// string literal
			Punct,			// punctuator
			Placemarker // empty token produced by ## with an empty operand
		};

		// string pool: equal spellings share one const String*, compare by pointer
		struct Interner {
			struct Slot {
				U64 hash = 0;
				const String* str = nullptr;
			};
			std::deque<String> store;
			List<Slot> slots = List<Slot>(4096);
			U64 count = 0;

			const String* intern(std::string_view s);
			void grow();
		};

		struct HideSet {
			List<const String*> names;
		};

		struct PpToken {
			const String* text = nullptr;	 // interned spelling
			const String* file = nullptr;	 // interned file name
			const HideSet* hide = nullptr; // interned hide set
			U32 line = 0;
			Pk kind = Pk::Eof;
			U8 spaceBefore = false; // had white space before it
			U8 bol = false;					// first token on its logical line
		};

		constexpr U32 kMaxIncludeDepth = 200;

		// 1-2 char fast paths avoid strlen/memcmp
		inline B32 isPunct(const PpToken& t, const C8* s) {
			if(t.kind != Pk::Punct)
				return false;
			const String& x = *t.text;
			if(s[1] == '\0')
				return x.size() == 1 && x[0] == s[0];
			if(s[2] == '\0')
				return x.size() == 2 && x[0] == s[0] && x[1] == s[1];
			return x == s;
		}
		String unquote(const String& s);
		B32 isAbsPath(const String& s);
		U64 ucnLen(const String& s, U64 i);
		Pk classify(const String& s);

		// absolute line correction at an output offset (emitted at splices)
		struct LineMark {
			U32 off;
			U32 line;
		};

		U64 decodeTrigraph(const String& src, U64 p, C8& c);
		U64 newlineLen(const String& s, U64 i);

		// 0x80 in each byte of w equal to b, 0 elsewhere
		constexpr U64 kBytes1 = 0x0101010101010101ull;
		U64 bytesEq(U64 w, C8 b);

		// trigraph + splice + newline norm in one copy; sparse LineMarks
		void splice(const String& src, String& out, List<LineMark>& marks);

		// non-owning token view (directive operands, expansion input)
		struct PpSpan {
			const PpToken* b = nullptr;
			const PpToken* e = nullptr;
			PpSpan(const List<PpToken>& v)
			: b(v.data()),
				e(v.data() + v.size()) {}
			PpSpan(const PpToken* pb, const PpToken* pe)
			: b(pb),
				e(pe) {}
			U64 size() const { return (U64)(e - b); }
			B32 empty() const { return b == e; }
			const PpToken& operator[](U64 i) const { return b[i]; }
		};

		String withSlash(String dir);
		String joinSpelling(PpSpan toks);
		List<String> stampDefs();

		struct LexResult {
			List<PpToken> toks;
			B32 ok = true;
			String err;
		};

		struct PpLexer {
			const String& s;
			const List<LineMark>& marks;
			const String* file;
			Interner& in;
			LexResult r;
			U64 mi = 0;
			U32 line = 1;
			B32 bolPending = true;
			B32 spacePending = false;

			void advanceTo(U64 p);
			void push(Pk kind, U64 start, U64 end);
			U64 skipComment(U64 i);
			U64 wordEnd(U64 j) const;
			U64 quotedEnd(U64 i) const;
			U64 numberEnd(U64 j) const;
			U64 lexToken(U64 i);
			LexResult run();
		};

		LexResult
		lexAll(const String& s, const List<LineMark>& marks, const String* file, Interner& in);

		using ArgLists = List<List<PpToken>>;

		// macros
		struct Macro {
			B32 isFunc = false;
			B32 variadic = false;
			const String* vaName = nullptr; // interned; set when variadic
			List<const String*> params;			// named parameters (excl variadic)
			List<const String*> formals;		// params plus vaName when variadic
			List<PpToken> body;
		};

		I32 formalIndex(const Macro& m, const String* s);

		// constexpr evaluator
		struct Val {
			U64 u = 0;
			B32 isU = false;
			B32 truth() const { return u != 0; }
		};

		Val truthVal(B32 r);
		I64 parseCharConst(const String& txt);
		Val parseNumLit(const String& txt);

		// #if expression evaluator
		struct Eval {
			const List<PpToken>& t;
			U64 i = 0;
			String& err;
			B32 ok = true;
			B32 live = true;

			Eval(const List<PpToken>& toks, String& e)
			: t(toks),
				err(e) {}

			const PpToken& cur() { return t[i]; }
			B32 isOp(const C8* s) { return isPunct(cur(), s); }
			B32 atEnd() { return cur().kind == Pk::Eof; }

			void fail(const String& m);
			Val parsePrimary();
			Val parseUnary();
			I32 prec(const String& op);
			Val apply(const String& op, Val a, Val b);
			Val parseBinary(I32 minPrec);
			Val parseExpr();
			Val run();
		};

		struct Preprocessor {
			const PpOptions& opts;
			Map<const String*, Macro> macros;
			List<PpToken> out;
			Set<String> pragmaOnce;
			Map<String, List<PpToken>> fileToks;
			struct SavedMacro {
				B32 defined;
				Macro macro;
			};
			Map<const String*, List<SavedMacro>> macroStack;
			U32 includeDepth = 0;
			// interned
			Interner interner;
			std::deque<HideSet> hideStore;
			Map<U64, List<const HideSet*>> hidePool; // keyed by pointer-FNV, chained
			// pre-interned, compared by pointer on hot paths
			const String* idLine = interner.intern("__LINE__");
			const String* idFile = interner.intern("__FILE__");
			const String* idDefined = interner.intern("defined");
			const String* idVaArgs = interner.intern("__VA_ARGS__");
			const String* idAttr = interner.intern("__attribute__");
			const String* idAttr2 = interner.intern("__attribute");
			const String* idNoinline = interner.intern("noinline");
			const String* idNoinline2 = interner.intern("__noinline__");
			const String* idNoinlineMark = interner.intern("__rat_noinline__");
			const String* idAligned = interner.intern("aligned");
			const String* idAligned2 = interner.intern("__aligned__");
			const String* idAlignas = interner.intern("_Alignas");
			const String* idAlias = interner.intern("alias");
			const String* idAlias2 = interner.intern("__alias__");
			const String* idAliasMark = interner.intern("__rat_alias__");
			const String* idPragma = interner.intern("_Pragma");
			String err;
			B32 ok = true;
			I64 lineDelta = 0;
			String fileName;

			struct Cond {
				B32 parentActive;
				B32 active;
				B32 taken;
				B32 sawElse = false;
			};

			explicit Preprocessor(const PpOptions& o)
			: opts(o) {}

			B32 isDefined(const String* name) {
				return macros.count(name) || name == idLine || name == idFile;
			}

			void fail(const String& m);

			const String* intern(const String& s) { return interner.intern(s); }
			const HideSet* internHide(List<const String*> names);
			static B32 hideHas(const HideSet* h, const String* name);
			const HideSet* hideInsert(const HideSet* h, const String* n);
			const HideSet* hideIntersect(const HideSet* a, const HideSet* b);
			const HideSet* hideUnion(const HideSet* a, const HideSet* b);

			// macro expansion
			PpToken makeNum(U64 v);
			PpToken makePunct(const String& s);
			List<PpToken> lexFragment(const String& text, const String* file);
			void pasteInto(PpToken& dst, const PpToken& r);
			PpToken stringize(const List<PpToken>& a, B32 spaceBefore);
			void appendList(List<PpToken>& os, List<PpToken> src, B32 firstSpace);
			void pasteArg(const List<PpToken>& a, B32 commaVa, List<PpToken>& os);
			List<PpToken> substitute(const Macro& m, const ArgLists& args, const HideSet* hs);
			List<PpToken> applyHideSet(List<PpToken>& os, const HideSet* hs);
			static U32 matchParen(const List<PpToken>& arg, U32 open);
			// stack: next token is work.back()
			B32 gatherArgs(List<PpToken>& work, ArgLists& raw, PpToken& rparen);
			B32 mapArgs(const Macro& m, const ArgLists& raw, ArgLists& actuals);
			void emitAttrMarkers(const PpToken& at, const ArgLists& raw, List<PpToken>& os);
			B32 expandBuiltinName(const PpToken& t, List<PpToken>& os);
			void requeueAlignas(const PpToken& at, const ArgLists& raw, List<PpToken>& work);
			void requeueExpansion(List<PpToken>& r, const PpToken& invoker, List<PpToken>& work);
			List<PpToken> expand(PpSpan in);

			// #if / #elif evaluation
			List<PpToken> replaceDefined(PpSpan in);
			B32 evalExpr(PpSpan toks);

			// directives
			B32 parseMacroParams(PpSpan toks, U64& i, Macro& m);
			B32 checkMacroBody(const Macro& m);
			void doDefine(PpSpan toks);
			void doUndef(PpSpan rest);
			void defineFragment(const String& text, const C8* file);
			static String dirOf(const String& path);
			B32 readFile(const String& path, String& content);
			B32 includeName(PpSpan toks, String& fname, B32& angled);
			U64 nextIncludeDir(const String& curDir);
			void enterInclude(const List<String>& tries, const String& fname);
			void doInclude(PpSpan rest, const String& path, B32 next);
			void doLine(PpSpan restIn, U32 physicalNextLine);
			void pushPopMacro(PpSpan rest, B32 push);
			void doPragma(PpSpan rest, const String& path);
			String destringize(const String& lit);
			List<PpToken> applyPragmaOperators(List<PpToken>& toks, const String& path);
			void flush(PpSpan text);
			static B32 condActive(const List<Cond>& stack);
			void pushCond(const String& name, PpSpan rest, List<Cond>& stack);
			B32 handleConditional(const String& name, PpSpan rest, List<Cond>& stack);
			void doDirective(PpSpan line, U32 phys, const String& path, List<Cond>& stack);

			// driver
			B32 run(const String& path, const String& source, String& errOut);
			void runFile(const String& path, const String& source);
			void runTokens(const String& path, const List<PpToken>& toks);
			void installBuiltins();
			void applyCommandLine();
			String serialize();
		};
	} // namespace detail
} // namespace rat::cc

#endif
