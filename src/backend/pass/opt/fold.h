// peephole constant folding and algebraic simplification, applied as local
// graph rewrites in the spirit of parse-time pessimistic peepholes
//
// references:
// - C. Click and M. Paleczny, "A Simple Graph-Based Intermediate
//   Representation", ACM SIGPLAN Workshop on IRs, 1995
// - C. Click, "Combining Analyses, Combining Optimizations", PhD thesis,
//   Rice University, 1995 (peephole rewriting on the sea-of-nodes graph)

#ifndef RAT_PASS_OPT_FOLD_H
#define RAT_PASS_OPT_FOLD_H

#include "core.h"
#include "ir/opcode.h"
#include "pass/pass.h"

namespace rat {
	struct Function;
	struct Module;
	struct Node;
	struct ConstantNode;
	struct Type;

	Node* constant(Function& fn, Type* type, I64 value);
	Node* mkBin(Function& fn, Opcode op, Type* ty, Node* x, I64 c);

	B32 evalBinaryConst(Opcode op, U32 w, I64 a, I64 b, I64& out);
	B32 evalUnaryConst(Opcode op, U32 w, I64 x, I64& out);
	B32 evalCompareConst(Opcode op, U32 w, I64 a, I64 b, I64& out);
	B32 evalConvertConst(Opcode op, U32 srcW, U32 dstW, I64 x, I64& out);

	Node* foldBinary(Function& fn, Opcode op, Node* lhs, Node* rhs);
	Node* foldUnary(Function& fn, Opcode op, Node* operand);
	Node* foldCompare(Function& fn, Opcode op, Node* lhs, Node* rhs);
	Node* foldConvert(Function& fn, Opcode op, Node* operand, Type* destType);
	Node* foldBinaryConst(Function& fn, Opcode op, Type* ty, U32 w, I64 a, I64 b);
	Node* foldBinaryIdentity(Function& fn, Opcode op, Type* ty, U32 w, Node* lhs, Node* rhs);
	Node* foldBinaryReassoc(Function& fn, Opcode op, Type* ty, U32 w, Node* lhs, ConstantNode* cr);
	Node* foldBinaryStrength(Function& fn, Opcode op, Type* ty, U32 w, Node* lhs, Node* rhs);
	Node* foldShiftOfShift(Function& fn, Opcode op, Type* ty, U32 w, Node* lhs, Node* rhs);

	// hacker's delight 10-9: magic number for unsigned 32-bit division by d
	struct MagicU32 {
		U32 m;
		U32 s;
		B32 a; // "add" indicator: the 33-bit constant case
	};
	MagicU32 magicU32(U32 d);

	// hacker's delight 10-4: magic number for signed 32-bit division by d
	struct MagicS32 {
		I32 m;
		U32 s;
	};
	MagicS32 magicS32(I32 d);

	Node* buildUDivByConst(Function& fn, Type* ty, Node* x, U32 d);
	Node* buildSDivByConst(Function& fn, Type* ty, Node* x, I32 d);
	Node* buildSDivByPow2(Function& fn, Type* ty, Node* x, I32 k);
	Node* buildDivByConst(Function& fn, Opcode op, Type* ty, U32 w, Node* x, ConstantNode* c);

	Node* simplify(Function& fn, Node* n); // dispatch to the matching fold*

	struct FoldPass : FunctionPass {
		const C8* name() const override;
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;

		static U64 maskW(I64 v, U32 w);									 // zero-extend low w bits
		static B32 wouldSignedDivOverflow(I64 a, I64 b); // div by 0 or INT_MIN-1
		static I64 normalizeConst(I64 v, U32 w);				 // canonical w-bit representative

		static B32 isConstWithValue(Node* n, I64 want);
		static B32 isZeroConst(Node* n);
		static B32 isOneConst(Node* n);
		static B32 isAllOnesConst(Node* n, U32 w);
		static I32 pow2Log(Node* n, U32 w);
		static B32 matchVarConst(Node* n, Opcode want, Node*& base, I64& c);
	private:
		List<Node*> work;
		List<char> queued;
		List<Node*> stack;
	};
} // namespace rat

#endif
