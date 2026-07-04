#ifndef RAT_PASS_EMIT_CEMITTER_H
#define RAT_PASS_EMIT_CEMITTER_H

#include "CodeGen/Schedule.h"
#include "Core.h"
#include "Support/Pass.h"

namespace rat {
	struct CallNode;
	struct Function;
	struct Global;
	struct Module;
	struct Node;
	struct PhiNode;
	struct Type;

	struct CEmitterPass : Pass {
		explicit CEmitterPass(std::ostream& os);

		const C8* name() const override;
		B32 run(Module& module) override;

		static void emitSignatureInto(const Function& fn, std::ostream& os);
		static String intCType(U32 width, B32 isSigned);
		static String cType(const Type* t, B32 isSigned = true);
		static B32 isCompilerBuiltin(const String& name);
	private:
		struct FunctionEmitter {
			const Function& fn;
			std::ostream& os;
			Schedule sched;
			Set<const Node*> needTemp;

			using TK = Schedule::TermKind;

			FunctionEmitter(const Function& fn, std::ostream& os);

			void run();

			static B32 producesTemp(const Node* n);
			void computeNeedTemp();

			String temp(const Node* n) const;
			String floatLiteral(Node* n);
			String valueExpr(Node* n);
			String binExpr(Node* n);
			String cmpExpr(Node* n);
			String convExpr(Node* n);
			String loadExpr(Node* n);

			void emit();
			void emitStatement(Node* n);
			static B32 isVoidBuiltin(const String& name);
			void emitCall(CallNode* c);
			B32 emitVaIntrinsic(CallNode* c, Node* valProj);

			struct Move {
				PhiNode* dst;
				Node* srcNode;
				String srcExpr;
			};

			List<Move> collectPhiMoves(I32 targetRegionB, I32 predIdx);
			void writeMoveBlock(const List<String>& scratchDecls, const List<String>& lines);
			void emitPhiCopies(I32 targetRegionB, I32 predIdx);
			void emitTerminator(I32 b);
		};

		void emitModule(const Module& module);
		void emitPrologue(const Module& module);
		void emitForwardDecls(const Module& module);
		void emitExternDecls(const Module& module);
		void emitGlobals(const Module& module);
		void emitRelocGlobal(const Global& g, U32 size, U32 ptrBytes);
		void emitFunction(const Function& fn);
		void emitSignature(const Function& fn);
	private:
		std::ostream* os;
	};
} // namespace rat

#endif
