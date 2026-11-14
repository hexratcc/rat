#include "emit/emit.h"

namespace rat::cc {
	Emitter::Value Emitter::emitPtrArith(Function& fn, ExprOp op, Value lhs, Value rhs) {
		if(op == ExprOp::Add) {
			if(isPointer(lhs.type) && isPointer(rhs.type))
				return diag.fail("invalid operands to binary '+' (two pointers)");
			Value p = isPointer(lhs.type) ? lhs : rhs;
			Value i = isPointer(lhs.type) ? rhs : lhs;
			if(!isInteger(i.type))
				return diag.fail("pointer arithmetic requires an integer operand");
			Node* idx = convert(fn, i.node, i.type, ctSize());
			Node* stride = elemStride(fn, p.type);
			if(!stride)
				return {};
			return {fn.add(p.node, fn.mul(idx, stride)), p.type};
		}
		if(op == ExprOp::Sub) {
			if(isPointer(lhs.type) && isPointer(rhs.type)) {
				CType pd = ctPtrDiff();
				Node* la = fn.convert(Opcode::SExt, lhs.node, irType(pd));
				Node* ra = fn.convert(Opcode::SExt, rhs.node, irType(pd));
				Node* diff = fn.sub(la, ra); // byte difference
				Node* stride = elemStride(fn, lhs.type);
				if(!stride)
					return {};
				return {fn.sdiv(diff, stride), pd};
			}
			if(!isPointer(lhs.type) || !isInteger(rhs.type))
				return diag.fail("invalid operands to binary '-'");
			Node* idx = convert(fn, rhs.node, rhs.type, ctSize());
			Node* stride = elemStride(fn, lhs.type);
			if(!stride)
				return {};
			return {fn.sub(lhs.node, fn.mul(idx, stride)), lhs.type};
		}
		return diag.fail("invalid operands to binary expression on a pointer");
	}

	Node* Emitter::emitArith(Function& fn, ExprOp op, Node* l, Node* r, CType ct) {
		if(isFloating(ct)) {
			if(op >= ExprOp::Add && op <= ExprOp::Div) // Add..Div map onto FAdd..FDiv
				return fn.binary((Opcode)((U32)Opcode::FAdd + ((U32)op - (U32)ExprOp::Add)), l, r);
			return diag.fail("invalid operator on a floating-point operand");
		}
		struct Sel {
			Opcode s, u;
		};
		constexpr Sel kInvalid = {Opcode::Start, Opcode::Start};
		// clang-format off
		static const Sel kArith[] = {
				{Opcode::Add, Opcode::Add},   // Add
				{Opcode::Sub, Opcode::Sub},   // Sub
				{Opcode::Mul, Opcode::Mul},   // Mul
				{Opcode::SDiv, Opcode::UDiv}, // Div
				{Opcode::SRem, Opcode::URem}, // Rem
				{Opcode::Shl, Opcode::Shl},   // Shl
				{Opcode::AShr, Opcode::LShr}, // Shr
				kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, // Lt..Ne
				{Opcode::And, Opcode::And},   // BitAnd
				{Opcode::Or, Opcode::Or},     // BitOr
				{Opcode::Xor, Opcode::Xor},   // BitXor
		};
		// clang-format on
		static_assert(sizeof(kArith) / sizeof(kArith[0]) == (U32)ExprOp::BitXor - (U32)ExprOp::Add + 1,
									"kArith must cover Add..BitXor");
		U32 idx = (U32)op - (U32)ExprOp::Add;
		if(op < ExprOp::Add || op > ExprOp::BitXor || kArith[idx].s == Opcode::Start)
			return diag.fail("unsupported arithmetic operator");
		Node* res = fn.binary(ct.isUnsigned() ? kArith[idx].u : kArith[idx].s, l, r);
		// signed overflow is undefined (C11 6.5p5). gcc defines a signed shl, so it gets no flag
		if(!ct.isUnsigned() && (op == ExprOp::Add || op == ExprOp::Sub || op == ExprOp::Mul))
			cast<BinaryNode>(res)->setNsw(true);
		return res;
	}

	Emitter::Value Emitter::emitIncDec(Function& fn, const Expr* e) {
		LValue lv;
		if(!emitLValue(fn, e->unary.operand, lv))
			return {};
		if(isTopConst(lv.type))
			return diag.fail("cannot modify a const-qualified lvalue");
		CType type = lv.type;
		ExprOp op = e->unary.op;
		B32 isInc = op == ExprOp::PreInc || op == ExprOp::PostInc;
		B32 isPre = op == ExprOp::PreInc || op == ExprOp::PreDec;
		if(isComplexType(type)) {
			CType ct = completeComplex(type);
			Value oldVal = {lv.addr, ct};
			Node* re = complexReal(fn, oldVal);
			Node* im = complexImag(fn, oldVal);
			Node* one = fn.constFloat(irType(complexElem(ct)), 1.0);
			Node* newRe = fn.binary(isInc ? Opcode::FAdd : Opcode::FSub, re, one);
			Value updated = makeComplex(fn, ct, newRe, im);
			storeComplex(fn, lv.addr, ct, updated);
			return isPre ? Value{lv.addr, ct} : makeComplex(fn, ct, re, im);
		}
		Node* old = loadLValue(fn, lv);
		Node* updated;
		if(isFloating(type)) {
			Node* delta = fn.constFloat(irType(type), 1.0);
			updated = fn.binary(isInc ? Opcode::FAdd : Opcode::FSub, old, delta);
		} else {
			Node* delta =
					isPointer(type) ? constSize(fn, byteSize(pointee(type))) : fn.constInt(irType(type), 1);
			updated = isInc ? fn.add(old, delta) : fn.sub(old, delta);
			// a signed type of int rank or more does not promote, so its overflow is undefined
			if(!isPointer(type) && !type.isUnsigned() && type.bits >= 32)
				cast<BinaryNode>(updated)->setNsw(true);
			updated = reduceBitfield(fn, updated, type);
		}
		storeLValue(fn, lv, updated);
		return {isPre ? updated : old, type};
	}

	Emitter::Value Emitter::assignStruct(Function& fn, const Expr* e, const LValue& lv, Value rhs) {
		CType targetType = lv.type;
		if(e->binary.op != ExprOp::Assign)
			return diag.fail("invalid compound assignment to a struct or union");
		if(!isStruct(rhs.type) || rhs.type.strukt != targetType.strukt)
			return diag.fail("assigning to '" + typeName(targetType) + "' from incompatible '" +
											 typeName(rhs.type) + "'");
		emitMemCopy(fn, lv.addr, rhs.node, targetType.strukt->size);
		return {lv.addr, targetType};
	}

	Emitter::Value Emitter::assignComplex(Function& fn, const Expr* e, const LValue& lv, Value rhs) {
		CType ct = completeComplex(lv.type);
		Value stored;
		if(e->binary.op == ExprOp::Assign) {
			stored = {rhs.node, rhs.type};
		} else {
			ExprOp base;
			B32 ok = compoundBaseOp(e->binary.op, base) && (base == ExprOp::Add || base == ExprOp::Sub ||
																											base == ExprOp::Mul || base == ExprOp::Div);
			if(!ok)
				return diag.fail("invalid compound assignment to a complex operand");
			CType opType = completeComplex(usualArithmetic(ct, rhs.type));
			Value cur = toComplex(fn, {lv.addr, ct}, opType);
			Value rc = toComplex(fn, rhs, opType);
			stored = complexBinary(fn, base, cur, rc, opType);
			if(!stored.node)
				return {};
		}
		storeComplex(fn, lv.addr, ct, stored);
		return {lv.addr, ct};
	}

	Emitter::Value Emitter::emitAssign(Function& fn, const Expr* e) {
		LValue lv;
		if(!emitLValue(fn, e->binary.lhs, lv))
			return {};
		if(lv.isArray)
			return diag.fail("array is not assignable");
		if(isTopConst(lv.type))
			return diag.fail("cannot assign to a const-qualified lvalue");
		CType targetType = lv.type;
		Value rhs = emitExpr(fn, e->binary.rhs);
		if(!rhs.node)
			return {};

		if(isStruct(targetType))
			return assignStruct(fn, e, lv, rhs);

		if(isComplexType(targetType))
			return assignComplex(fn, e, lv, rhs);

		Node* stored = nullptr;
		if(e->binary.op == ExprOp::Assign) {
			stored = convert(fn, rhs.node, rhs.type, targetType);
		} else if(isPointer(targetType) &&
							(e->binary.op == ExprOp::AddAssign || e->binary.op == ExprOp::SubAssign)) {
			ExprOp op = e->binary.op == ExprOp::AddAssign ? ExprOp::Add : ExprOp::Sub;
			Value res = emitPtrArith(fn, op, {loadLValue(fn, lv), targetType}, rhs);
			if(!res.node)
				return {};
			stored = res.node;
		} else {
			ExprOp base;
			if(!compoundBaseOp(e->binary.op, base))
				return diag.fail("unsupported assignment operator");
			CType ct = usualArithmetic(targetType, rhs.type);
			Node* l = convert(fn, loadLValue(fn, lv), targetType, ct);
			Node* r = convert(fn, rhs.node, rhs.type, ct);
			Node* res = emitArith(fn, base, l, r, ct);
			if(!res)
				return {};
			stored = convert(fn, res, ct, targetType);
		}
		storeLValue(fn, lv, stored);
		return {stored, targetType};
	}

	Emitter::Value Emitter::emitLogicalBinary(Function& fn, const Expr* e) {
		B32 isAnd = e->binary.op == ExprOp::LogAnd;
		Value lhs = emitExpr(fn, e->binary.lhs);
		if(!lhs.node)
			return {};
		if(isAggregate(lhs.type) && !isComplexType(lhs.type))
			return diag.fail("operand of logical operator must have scalar type");
		Node* lpred = toBool(fn, lhs);
		Function::Var var = fn.newVar("logic", i32);
		Block* rhsB = fn.createBlock(isAnd ? "and.rhs" : "or.rhs");
		Block* shortB = fn.createBlock(isAnd ? "and.short" : "or.short");
		Block* endB = fn.createBlock(isAnd ? "and.end" : "or.end");
		fn.jumpif(lpred, isAnd ? rhsB : shortB);
		fn.jmp(isAnd ? shortB : rhsB);
		fn.enterBlock(rhsB);
		Value rhs = emitExpr(fn, e->binary.rhs);
		if(!rhs.node)
			return {};
		if(isAggregate(rhs.type) && !isComplexType(rhs.type))
			return diag.fail("operand of logical operator must have scalar type");
		fn.set(var, fromBool(fn, toBool(fn, rhs)));
		fn.jmp(endB);
		fn.enterBlock(shortB);
		fn.set(var, fn.constInt(i32, isAnd ? 0 : 1));
		fn.jmp(endB);
		fn.enterBlock(endB);
		return {fn.get(var), ctInt()};
	}

	Emitter::Value Emitter::emitComparison(Function& fn, ExprOp op, Value lhs, Value rhs) {
		B32 ptrCmp = isPointer(lhs.type) || isPointer(rhs.type);
		CType ct = ptrCmp ? ctSize() : usualArithmetic(lhs.type, rhs.type);
		Node* l = ptrCmp ? lhs.node : convert(fn, lhs.node, lhs.type, ct);
		Node* r = ptrCmp ? rhs.node : convert(fn, rhs.node, rhs.type, ct);
		struct Sel {
			Opcode f, s, u;
			B32 swap;
		};
		// clang-format off
		static const Sel kCmp[] = {
				{Opcode::FLt, Opcode::Slt, Opcode::Ult, false}, // Lt
				{Opcode::FGt, Opcode::Slt, Opcode::Ult, true},  // Gt
				{Opcode::FLe, Opcode::Sle, Opcode::Ule, false}, // Le
				{Opcode::FGe, Opcode::Sle, Opcode::Ule, true},  // Ge
				{Opcode::FEq, Opcode::Eq, Opcode::Eq, false},   // Eq
				{Opcode::FNe, Opcode::Ne, Opcode::Ne, false},   // Ne
		};
		// clang-format on
		static_assert(sizeof(kCmp) / sizeof(kCmp[0]) == (U32)ExprOp::Ne - (U32)ExprOp::Lt + 1,
									"kCmp must cover Lt..Ne");
		const Sel& sel = kCmp[(U32)op - (U32)ExprOp::Lt]; // caller guarantees Lt..Ne
		Node* cmp;
		if(isFloating(ct))
			cmp = fn.compare(sel.f, l, r);
		else if(sel.swap)
			cmp = fn.compare(ct.isUnsigned() ? sel.u : sel.s, r, l);
		else
			cmp = fn.compare(ct.isUnsigned() ? sel.u : sel.s, l, r);
		return {fromBool(fn, cmp), ctInt()};
	}

	Emitter::Value Emitter::emitComplexBinary(Function& fn, ExprOp op, Value lhs, Value rhs) {
		CType ct = completeComplex(usualArithmetic(lhs.type, rhs.type));
		Value lc = toComplex(fn, lhs, ct);
		Value rc = toComplex(fn, rhs, ct);
		if(op != ExprOp::Eq && op != ExprOp::Ne)
			return complexBinary(fn, op, lc, rc, ct);
		Node* re = fn.compare(Opcode::FEq, complexReal(fn, lc), complexReal(fn, rc));
		Node* im = fn.compare(Opcode::FEq, complexImag(fn, lc), complexImag(fn, rc));
		Node* eq = fn.and_(re, im);
		Node* res = op == ExprOp::Eq ? eq : fn.eq(eq, fn.constInt(i32, 0));
		return {fromBool(fn, res), ctInt()};
	}

	// each operand promotes on its own, the result has the left type
	Emitter::Value Emitter::emitShift(Function& fn, ExprOp op, Value lhs, Value rhs) {
		CType lt = promote(lhs.type);
		CType rt = promote(rhs.type);
		Node* l = convert(fn, lhs.node, lhs.type, lt);
		Node* r = convert(fn, rhs.node, rhs.type, rt);
		Node* res = emitArith(fn, op, l, r, lt);
		if(!res)
			return {};
		return {reduceBitfield(fn, res, lt), lt};
	}

	Emitter::Value Emitter::emitBinary(Function& fn, const Expr* e) {
		ExprOp op = e->binary.op;
		if(isAssignOp(op))
			return emitAssign(fn, e);
		if(op == ExprOp::LogAnd || op == ExprOp::LogOr)
			return emitLogicalBinary(fn, e);

		Value lhs = emitExpr(fn, e->binary.lhs);
		if(!lhs.node)
			return {};
		Value rhs = emitExpr(fn, e->binary.rhs);
		if(!rhs.node)
			return {};
		if(isComplexType(lhs.type) || isComplexType(rhs.type))
			return emitComplexBinary(fn, op, lhs, rhs);
		if(isIntegerOnlyOp(op) && (!isInteger(lhs.type) || !isInteger(rhs.type)))
			return diag.fail("operands of this operator must have integer type");

		switch(op) {
		case ExprOp::Shl:
		case ExprOp::Shr:
			return emitShift(fn, op, lhs, rhs);
		case ExprOp::Lt:
		case ExprOp::Gt:
		case ExprOp::Le:
		case ExprOp::Ge:
		case ExprOp::Eq:
		case ExprOp::Ne:
			return emitComparison(fn, op, lhs, rhs);
		default:
			break;
		}

		if(isPointer(lhs.type) || isPointer(rhs.type))
			return emitPtrArith(fn, op, lhs, rhs);

		CType ct = usualArithmetic(lhs.type, rhs.type);
		Node* l = convert(fn, lhs.node, lhs.type, ct);
		Node* r = convert(fn, rhs.node, rhs.type, ct);
		Node* res = emitArith(fn, op, l, r, ct);
		if(!res)
			return {};
		return {reduceBitfield(fn, res, ct), ct};
	}
} // namespace rat::cc
