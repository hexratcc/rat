#ifndef RAT_CC_PREPROCESS_DETAIL_H
#define RAT_CC_PREPROCESS_DETAIL_H

#include <deque>

#include "Preprocess.h"

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

		struct PpToken {
			Pk kind = Pk::Eof;
			String text;
			B32 spaceBefore = false; // had white space before it
			B32 bol = false;				 // first token on its logical line
			U32 line = 0;
			String file;
			Set<String> hide; // hide set (macros that must not re-expand here)
		};

		constexpr U32 kMaxIncludeDepth = 200;

		B32 isPunct(const PpToken& t, const char* s);
		String unquote(const String& s);
		void stripTrailingSlash(String& s);
		size_t ucnLen(const String& s, size_t i);
		Pk classify(const String& s);
		String trigraph(const String& src);
		void splice(const String& src, String& out, List<U32>& lineOf);

		struct LexResult {
			List<PpToken> toks;
			B32 ok = true;
			String err;
		};

		LexResult lexAll(const String& s, const List<U32>& lineOf, const String& file);

		// macros
		struct Macro {
			B32 isFunc = false;
			B32 variadic = false;
			String vaName = "__VA_ARGS__";
			List<String> params; // named parameters (excl variadic)
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
			Map<String, Macro> macros;
			List<PpToken> out;
			Set<String> pragmaOnce;
			struct SavedMacro {
				B32 defined;
				Macro macro;
			};
			Map<String, List<SavedMacro>> macroStack;
			U32 includeDepth = 0;
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

			B32 isBuiltinDynamic(const String& name) { return name == "__LINE__" || name == "__FILE__"; }
			B32 isDefined(const String& name) { return macros.count(name) || isBuiltinDynamic(name); }

			void fail(const String& m);

			// macro expansion
			static PpToken makeNum(U64 v);
			static PpToken makePunct(const String& s);
			List<PpToken> lexFragment(const String& text, const String& file);
			static void pasteInto(PpToken& dst, const PpToken& r);
			PpToken stringize(const List<PpToken>& a, B32 spaceBefore);
			void appendList(List<PpToken>& os, List<PpToken> src, B32 firstSpace);
			List<PpToken> substitute(const Macro& m,
															 const List<List<PpToken>>& args,
															 const Set<String>& hs,
															 const List<String>& formals);
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
