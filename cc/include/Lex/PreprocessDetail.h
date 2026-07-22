#ifndef RAT_CC_PREPROCESS_DETAIL_H
#define RAT_CC_PREPROCESS_DETAIL_H

#include <deque>
#include <string_view>

#include "Lex/Preprocess.h"

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
			std::deque<String> store;
			Map<std::string_view, const String*> pool;

			const String* intern(std::string_view s) {
				auto it = pool.find(s);
				if(it != pool.end())
					return it->second;
				store.emplace_back(s);
				const String* p = &store.back();
				pool.emplace(std::string_view(*p), p);
				return p;
			}
		};

		struct HideSet {
			List<const String*> names;
		};

		struct PpToken {
			Pk kind = Pk::Eof;
			const String* text = nullptr;	 // interned spelling
			B32 spaceBefore = false;			 // had white space before it
			B32 bol = false;							 // first token on its logical line
			U32 line = 0;
			const String* file = nullptr;	 // interned file name
			const HideSet* hide = nullptr; // interned hide set
		};

		constexpr U32 kMaxIncludeDepth = 200;

		B32 isPunct(const PpToken& t, const char* s);
		String unquote(const String& s);
		void stripTrailingSlash(String& s);
		size_t ucnLen(const String& s, size_t i);
		Pk classify(const String& s);

		// absolute line correction at an output offset (emitted at splices)
		struct LineMark {
			U32 off;
			U32 line;
		};

		// trigraph + splice + newline norm in one copy; sparse LineMarks
		void splice(const String& src, String& out, List<LineMark>& marks);

		struct LexResult {
			List<PpToken> toks;
			B32 ok = true;
			String err;
		};

		LexResult
		lexAll(const String& s, const List<LineMark>& marks, const String* file, Interner& in);

		// macros
		struct Macro {
			B32 isFunc = false;
			B32 variadic = false;
			const String* vaName = nullptr; // interned; set when variadic
			List<const String*> params;			// named parameters (excl variadic)
			List<PpToken> body;
		};

		// constexpr evaluator
		struct Val {
			U64 u = 0;
			B32 isU = false;
			B32 truth() const { return u != 0; }
		};

		I64 parseCharConst(const String& txt);
		Val parseNumLit(const String& txt);

		// #if expression evaluator
		struct Eval {
			const List<PpToken>& t;
			size_t i = 0;
			String& err;
			B32 ok = true;
			B32 live = true;

			Eval(const List<PpToken>& toks, String& e)
			: t(toks),
				err(e) {}

			const PpToken& cur() { return t[i]; }
			B32 isOp(const char* s) { return isPunct(cur(), s); }
			B32 atEnd() { return cur().kind == Pk::Eof; }

			void fail(const String& m);
			Val parsePrimary();
			Val parseUnary();
			int prec(const String& op);
			Val apply(const String& op, Val a, Val b);
			Val parseBinary(int minPrec);
			Val parseExpr();
			Val run();
		};

		struct Preprocessor {
			const PpOptions& opts;
			Map<const String*, Macro> macros;
			List<PpToken> out;
			Set<String> pragmaOnce;
			struct SavedMacro {
				B32 defined;
				Macro macro;
			};
			Map<const String*, List<SavedMacro>> macroStack;
			U32 includeDepth = 0;
			// interned
			Interner interner;
			std::deque<HideSet> hideStore;
			Map<String, const HideSet*> hidePool;
			// pre-interned, compared by pointer on hot paths
			const String* idLine;		 // "__LINE__"
			const String* idFile;		 // "__FILE__"
			const String* idDefined; // "defined"
			const String* idVaArgs;	 // "__VA_ARGS__"
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
			: opts(o) {
				idLine = interner.intern("__LINE__");
				idFile = interner.intern("__FILE__");
				idDefined = interner.intern("defined");
				idVaArgs = interner.intern("__VA_ARGS__");
			}

			B32 isBuiltinDynamic(const String* name) { return name == idLine || name == idFile; }
			B32 isDefined(const String* name) { return macros.count(name) || isBuiltinDynamic(name); }

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
			List<PpToken> substitute(const Macro& m,
															 const List<List<PpToken>>& args,
															 const HideSet* hs,
															 const List<const String*>& formals);
			B32 gatherArgs(std::deque<PpToken>& work, List<List<PpToken>>& raw, PpToken& rparen);
			B32 mapArgs(const Macro& m, const List<List<PpToken>>& raw, List<List<PpToken>>& actuals);
			void requeueExpansion(List<PpToken>& r, const PpToken& invoker, std::deque<PpToken>& work);
			List<PpToken> expand(List<PpToken> in);

			// #if / #elif evaluation
			List<PpToken> replaceDefined(const List<PpToken>& in);
			B32 evalExpr(const List<PpToken>& toks);

			// directives
			void doDefine(const List<PpToken>& toks);
			void defineSimple(const String& name, const String& value);
			static String dirOf(const String& path);
			B32 readFile(const String& path, String& content);
			void doInclude(const List<PpToken>& restIn, const String& curDir, B32 next = false);
			void doLine(const List<PpToken>& restIn, U32 physicalNextLine);
			void doPragma(const List<PpToken>& rest, const String& path);
			String destringize(const String& lit);
			List<PpToken> applyPragmaOperators(List<PpToken>& toks, const String& path);
			void flush(List<PpToken>& textBuf);
			static B32 condActive(const List<Cond>& stack);
			B32 handleConditional(const String& name, const List<PpToken>& rest, List<Cond>& stack);

			// driver
			void runFile(const String& path, const String& source);
			void installBuiltins();
			void applyCommandLine();
			String serialize();
		};
	} // namespace detail
} // namespace rat::cc

#endif
