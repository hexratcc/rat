#include "parse/const_eval.h"

namespace rat::cc {
	namespace detail {
		// truncated toward zero, a _Bool keeps the truth value
		// false for NaN and values no 64 bit integer holds
		B32 floatToInt(F80 v, CType to, I64& out) {
			if(to.bits == 1) {
				out = v != 0;
				return true;
			}
			if(!(v > -0x1p63L - 1 && v < 0x1p64L))
				return false;
			if(v >= 0x1p63L)
				out = (I64)(U64)v;
			else
				out = (I64)v;
			return true;
		}
	} // namespace detail

	// an integer or pointer constant
	B32 ConstEval::evalInt(const Expr* e, I64& out) const {
		CType t = valueType(e->type);
		if((!isInteger(t) && !isPointer(t)) || !evalIntNode(e, out))
			return false;
		out = wrapTo(out, t);
		return true;
	}

	// an integer operand converts, see evalInt
	B32 ConstEval::evalFloat(const Expr* e, F80& out) const {
		CType t = valueType(e->type);
		if(isInteger(t)) {
			I64 v;
			if(!evalInt(e, v))
				return false;
			out = t.isUnsigned() ? (F80)(U64)v : (F80)v;
			return true;
		}
		if(!isFloating(t) || !evalFloatNode(e, out))
			return false;
		if(t.bits == 32) // a float or double rounds once to its own precision
			out = (F32)out;
		else if(t.bits == 64)
			out = (F64)out;
		return true;
	}

	// false for a VLA, an unsized array and an incomplete struct
	B32 ConstEval::sizeOf(const Expr* e, U64& out) const {
		CType t = e->sizeOf.type;
		if(e->sizeOf.operand)
			t = e->sizeOf.operand->type;
		if(t.func && t.ptr == 0) {
			out = 1; // as gcc
			return true;
		}
		if(e->kind == ExprKind::AlignOf) {
			out = typeAlign(t, lay.ptrBytes);
			return true;
		}
		if(hasVlaDim(t) || isUnsizedArray(t) || (isStruct(t) && !t.strukt->complete))
			return false;
		out = typeSize(t, lay.ptrBytes);
		return true;
	}

	B32 ConstEval::evalIntNode(const Expr* e, I64& out) const {
		switch(e->kind) {
		case ExprKind::IntLit:
			out = e->intLit.value;
			return true;
		case ExprKind::Sizeof:
		case ExprKind::AlignOf: {
			U64 n;
			if(!sizeOf(e, n))
				return false;
			out = (I64)n;
			return true;
		}
		case ExprKind::Cast:
			return evalAs(e->cast.operand, e->cast.type, out);
		case ExprKind::Unary:
			return evalUnary(e, out);
		case ExprKind::Binary:
			return evalBinary(e, out);
		case ExprKind::Ternary: {
			B32 cond;
			if(!evalTruth(e->ternary.cond, cond))
				return false;
			return evalInt(cond ? e->ternary.whenTrue : e->ternary.whenFalse, out);
		}
		case ExprKind::Comma:
			return evalInt(e->comma.rhs, out);
		case ExprKind::Call:
			if(!e->call.callee || *e->call.callee != "__builtin_classify_type" || e->args.size() != 1)
				return false;
			out = typeClassOf(valueType(e->args[0]->type));
			return true;
		default:
			return false; // an enum constant is an IntLit already
		}
	}

	B32 ConstEval::evalFloatNode(const Expr* e, F80& out) const {
		switch(e->kind) {
		case ExprKind::FloatLit:
			out = e->floatLit.value;
			return true;
		case ExprKind::Cast:
			return evalFloat(e->cast.operand, out);
		case ExprKind::Unary: {
			ExprOp op = e->unary.op;
			if((op != ExprOp::Pos && op != ExprOp::Neg) || !evalFloat(e->unary.operand, out))
				return false;
			if(op == ExprOp::Neg)
				out = -out;
			return true;
		}
		case ExprKind::Binary: {
			F80 a;
			F80 b;
			if(!evalFloat(e->binary.lhs, a) || !evalFloat(e->binary.rhs, b))
				return false;
			switch(e->binary.op) {
				// clang-format off
			case ExprOp::Add: out = a + b; return true;
			case ExprOp::Sub: out = a - b; return true;
			case ExprOp::Mul: out = a * b; return true;
			case ExprOp::Div: out = a / b; return true;
			// clang-format on
			default:
				return false;
			}
		}
		case ExprKind::Ternary: {
			B32 cond;
			if(!evalTruth(e->ternary.cond, cond))
				return false;
			return evalFloat(cond ? e->ternary.whenTrue : e->ternary.whenFalse, out);
		}
		default:
			return false;
		}
	}

	// a scalar condition, integer, pointer or floating
	B32 ConstEval::evalTruth(const Expr* e, B32& out) const {
		I64 v;
		if(evalInt(e, v)) {
			out = v != 0;
			return true;
		}
		F80 f;
		if(!evalFloat(e, f))
			return false;
		out = f != 0;
		return true;
	}

	// e converted to an integer or pointer type, as a cast would
	B32 ConstEval::evalAs(const Expr* e, CType to, I64& out) const {
		if(!isInteger(to) && !isPointer(to))
			return false;
		if(!isFloating(valueType(e->type))) {
			if(!evalInt(e, out))
				return false;
		} else {
			F80 v;
			if(isPointer(to) || !evalFloat(e, v) || !detail::floatToInt(v, to, out))
				return false;
		}
		out = wrapTo(out, to);
		return true;
	}

	B32 ConstEval::evalUnary(const Expr* e, I64& out) const {
		ExprOp op = e->unary.op;
		if(op == ExprOp::Not) {
			B32 truth;
			if(!evalTruth(e->unary.operand, truth))
				return false;
			out = !truth;
			return true;
		}
		I64 v;
		if(!evalInt(e->unary.operand, v))
			return false;
		switch(op) {
			// clang-format off
		case ExprOp::Pos:    out = v;                  return true;
		case ExprOp::Neg:    out = (I64)(0 - (U64)v);  return true;
		case ExprOp::BitNot: out = ~v;                 return true;
		// clang-format on
		default:
			return false;
		}
	}

	// operands convert to their common type first
	B32 ConstEval::evalBinary(const Expr* e, I64& out) const {
		ExprOp op = e->binary.op;
		if(op == ExprOp::LogAnd || op == ExprOp::LogOr)
			return evalLogical(e, out);
		I64 a;
		I64 b;
		if(!evalInt(e->binary.lhs, a) || !evalInt(e->binary.rhs, b))
			return false;
		CType at = valueType(e->binary.lhs->type);
		CType bt = valueType(e->binary.rhs->type);
		if(isPointer(at) || isPointer(bt))
			return evalPointerOp(op, a, at, b, bt, out);
		if(op == ExprOp::Shl || op == ExprOp::Shr)
			return evalShift(op, a, b, valueType(e->type), out);
		CType common = usualArithmetic(at, bt);
		return evalArith(op, wrapTo(a, common), wrapTo(b, common), common.isUnsigned(), out);
	}

	// the side the lhs decides is not evaluated
	B32 ConstEval::evalLogical(const Expr* e, I64& out) const {
		B32 lhs;
		if(!evalTruth(e->binary.lhs, lhs))
			return false;
		if(lhs != (e->binary.op == ExprOp::LogAnd)) { // 0 && x or 1 || x
			out = lhs;
			return true;
		}
		B32 rhs;
		if(!evalTruth(e->binary.rhs, rhs))
			return false;
		out = rhs;
		return true;
	}

	// address arithmetic and unsigned address compares
	B32 ConstEval::evalPointerOp(ExprOp op, I64 a, CType at, I64 b, CType bt, I64& out) const {
		if(op >= ExprOp::Lt && op <= ExprOp::Ne) {
			CType addr = isPointer(at) ? at : bt;
			return evalArith(op, wrapTo(a, addr), wrapTo(b, addr), true, out);
		}
		if(isPointer(at) && isPointer(bt)) {
			U64 elemBytes = typeSize(pointee(at), lay.ptrBytes);
			if(op != ExprOp::Sub || !elemBytes)
				return false;
			out = (I64)((U64)a - (U64)b) / (I64)elemBytes;
			return true;
		}
		if(op == ExprOp::Sub && !isPointer(at))
			return false;
		if(op != ExprOp::Add && op != ExprOp::Sub)
			return false;
		if(!isPointer(at)) { // int + ptr
			std::swap(a, b);
			std::swap(at, bt);
		}
		U64 offset = (U64)b * typeSize(pointee(at), lay.ptrBytes);
		if(op == ExprOp::Sub)
			offset = 0 - offset;
		out = (I64)((U64)a + offset);
		return true;
	}

	// rt is the promoted lhs, a count outside its width is not a constant
	B32 ConstEval::evalShift(ExprOp op, I64 a, I64 b, CType rt, I64& out) const {
		if(b < 0 || b >= (I64)rt.bits)
			return false;
		if(op == ExprOp::Shl)
			out = (I64)((U64)a << b);
		else if(rt.isUnsigned())
			out = (I64)((U64)a >> b);
		else
			out = a >> b;
		return true;
	}

	// a and b are wrapped to their common type already, the caller wraps the result
	B32 ConstEval::evalArith(ExprOp op, I64 a, I64 b, B32 isUnsigned, I64& out) const {
		U64 ua = (U64)a;
		U64 ub = (U64)b;
		switch(op) {
			// clang-format off
		case ExprOp::Add:    out = (I64)(ua + ub);           return true;
		case ExprOp::Sub:    out = (I64)(ua - ub);           return true;
		case ExprOp::Mul:    out = (I64)(ua * ub);           return true;
		case ExprOp::BitAnd: out = a & b;                    return true;
		case ExprOp::BitOr:  out = a | b;                    return true;
		case ExprOp::BitXor: out = a ^ b;                    return true;
		case ExprOp::Lt:     out = isUnsigned ? ua < ub : a < b;   return true;
		case ExprOp::Gt:     out = isUnsigned ? ua > ub : a > b;   return true;
		case ExprOp::Le:     out = isUnsigned ? ua <= ub : a <= b; return true;
		case ExprOp::Ge:     out = isUnsigned ? ua >= ub : a >= b; return true;
		case ExprOp::Eq:     out = a == b;                   return true;
		case ExprOp::Ne:     out = a != b;                   return true;
		// clang-format on
		case ExprOp::Div:
		case ExprOp::Rem:
			return evalDivision(op, a, b, isUnsigned, out);
		default:
			return false;
		}
	}

	// by zero is not a constant, nor INT64_MIN / -1 which traps
	B32 ConstEval::evalDivision(ExprOp op, I64 a, I64 b, B32 isUnsigned, I64& out) const {
		if(!b || (!isUnsigned && a == INT64_MIN && b == -1))
			return false;
		B32 isDiv = op == ExprOp::Div;
		if(isUnsigned && isDiv)
			out = (I64)((U64)a / (U64)b);
		else if(isUnsigned)
			out = (I64)((U64)a % (U64)b);
		else if(isDiv)
			out = a / b;
		else
			out = a % b;
		return true;
	}

	// a pointer is an unsigned address
	I64 ConstEval::wrapTo(I64 v, CType t) const {
		if(isPointer(t))
			return wrapInt(v, lay.ptrBytes * 8, true);
		return wrapInt(v, t.bits, t.isUnsigned());
	}

	// gcc's type classes for __builtin_classify_type
	I64 typeClassOf(CType t) {
		if(isPointer(t) || isArrayType(t) || t.func != nullptr)
			return 5; // pointer_type_class
		if(isVoidType(t))
			return 0; // void_type_class
		if(isComplexType(t))
			return 9; // complex_type_class
		if(isStruct(t))
			return t.strukt->isUnion ? 13 : 12; // union_ / record_type_class
		if(isFloating(t))
			return 8; // real_type_class
		return 1;		// integer_type_class
	}

	// unsigned long, or unsigned long long where long is 32 bits
	CType sizeType(const TargetLayout& lay) {
		CType t = ptrDiffType(lay);
		t.set(CType::Unsigned);
		return t;
	}

	// long, or long long where long is 32 bits
	CType ptrDiffType(const TargetLayout& lay) {
		CType t;
		t.bits = lay.ptrBytes * 8;
		t.set(CType::Long);
		t.set(CType::LongLong, lay.longBits < 64);
		return t;
	}
} // namespace rat::cc
