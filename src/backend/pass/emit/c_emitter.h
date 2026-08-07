#ifndef RAT_PASS_EMIT_CEMITTER_H
#define RAT_PASS_EMIT_CEMITTER_H

#include "codegen/schedule.h"
#include "core.h"
#include "pass/pass.h"

namespace rat {
	struct CallNode;
	struct Function;
	struct Global;
	struct Module;
	struct Node;
	struct PhiNode;
	struct TargetInfo;
	struct Type;

	struct CEmitterPass : Pass {
		explicit CEmitterPass(std::ostream& os);

		const C8* name() const override;
		B32 run(Module& module, const TargetInfo& target) override;

		static void emitSignatureInto(const Function& fn, std::ostream& os);
		static const C8* intCType(U32 width, B32 isSigned);
		static const C8* cType(const Type* t, B32 isSigned = true);
		static B32 isCompilerBuiltin(const String& name);
		static const C8* variadicExternProto(const String& name);
	private:
		struct FunctionEmitter {
			const Function& fn;
			std::ostream& os;
			U32 ptrBytes;
			Schedule sched;
			Set<const Node*> needTemp;

			using TK = Schedule::TermKind;

			FunctionEmitter(const Function& fn, std::ostream& os, U32 ptrBytes);

			void run();

			static B32 producesTemp(const Node* n);
			void computeNeedTemp();

			void writeTemp(std::ostream& os, const Node* n) const;
			void writeFloatLiteral(std::ostream& os, Node* n);
			void writeValue(std::ostream& os, Node* n);
			void writeBin(std::ostream& os, Node* n);
			void writeCmp(std::ostream& os, Node* n);
			void writeConv(std::ostream& os, Node* n);
			void writeLoad(std::ostream& os, Node* n);
			void writeArgs(std::ostream& os, CallNode* c);

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

		void emitModule(const Module& module, const TargetInfo& target);
		void emitPrologue(const TargetInfo& target);
		void emitForwardDecls(const Module& module);
		void emitExternDecls(const Module& module);
		void emitGlobals(const Module& module, U32 ptrBytes);
		void emitRelocGlobal(const Global& g, U32 size, U32 ptrBytes);
		void emitFunction(const Function& fn, U32 ptrBytes);
		void emitSignature(const Function& fn);
	private:
		std::ostream* os;
	};
} // namespace rat

#endif
