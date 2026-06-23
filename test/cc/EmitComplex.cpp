#include "Emit.h"

#include "IR/Function.h"
#include "IR/Module.h"

namespace rat::cc {
	CType Emitter::completeComplex(CType t) {
		if (!isComplexType(t) || t.strukt != nullptr)
			return t;
		auto it = complexLayouts.find(t.bits);
		StructType* st;
		if (it != complexLayouts.end())
			st = it->second;
		else {
			U32 elemBytes = (t.bits + 7) / 8;
			CType elem = t;
			elem.isComplex = false;
			elem.strukt = nullptr;
			st = arena.make<StructType>();
			st->size = 2 * elemBytes;
			st->align = elemBytes;
			st->complete = true;
			Field re;
			re.type = elem;
			re.offset = 0;
			Field im;
			im.type = elem;
			im.offset = elemBytes;
			st->fields.push_back(re);
			st->fields.push_back(im);
			complexLayouts[t.bits] = st;
		}
		t.strukt = st;
		return t;
	}

	Node* Emitter::complexReal(Function& fn, const Value& v) {
		CType re = complexElem(v.type);
		return fn.load(irType(re), v.node);
	}

	Node* Emitter::complexImag(Function& fn, const Value& v) {
		CType re = complexElem(v.type);
		U32 off = byteSize(re);
		return fn.load(irType(re), offsetPtr(fn, v.node, off));
	}

	Emitter::Value Emitter::makeComplex(Function& fn, CType type, Node* re,
																			Node* im) {
		type = completeComplex(type);
		Node* slot = allocBytes(fn, type.strukt->size);
		fn.store(slot, re);
		fn.store(offsetPtr(fn, slot, byteSize(complexElem(type))), im);
		return {slot, type};
	}

	Emitter::Value Emitter::toComplex(Function& fn, const Value& v, CType type) {
		CType re = complexElem(type);
		if (isComplexType(v.type)) {
			Node* r = convert(fn, complexReal(fn, v), complexElem(v.type), re);
			Node* i = convert(fn, complexImag(fn, v), complexElem(v.type), re);
			return makeComplex(fn, type, r, i);
		}
		Node* r = convert(fn, v.node, v.type, re);
		return makeComplex(fn, type, r, fn.constFloat(irType(re), 0.0));
	}

	void Emitter::storeComplex(Function& fn, Node* addr, CType type,
														 const Value& v) {
		Value c = toComplex(fn, v, type);
		emitMemCopy(fn, addr, c.node, byteSize(type));
	}

	Emitter::Value Emitter::emitComplexBinary(Function& fn, ExprOp op, Value lhs,
																						Value rhs, CType ct) {
		Node* a = complexReal(fn, lhs);
		Node* b = complexImag(fn, lhs);
		Node* c = complexReal(fn, rhs);
		Node* d = complexImag(fn, rhs);
		switch (op) {
		case ExprOp::Add:
			return makeComplex(fn, ct, fn.binary(Opcode::FAdd, a, c),
												 fn.binary(Opcode::FAdd, b, d));
		case ExprOp::Sub:
			return makeComplex(fn, ct, fn.binary(Opcode::FSub, a, c),
												 fn.binary(Opcode::FSub, b, d));
		case ExprOp::Mul: {
			// (a+bi)(c+di) = (ac - bd) + (ad + bc)i
			Node* ac = fn.binary(Opcode::FMul, a, c);
			Node* bd = fn.binary(Opcode::FMul, b, d);
			Node* ad = fn.binary(Opcode::FMul, a, d);
			Node* bc = fn.binary(Opcode::FMul, b, c);
			return makeComplex(fn, ct, fn.binary(Opcode::FSub, ac, bd),
												 fn.binary(Opcode::FAdd, ad, bc));
		}
		case ExprOp::Div: {
			// (a+bi)/(c+di) = ((ac+bd) + (bc-ad)i) / (c*c + d*d)
			Node* ac = fn.binary(Opcode::FMul, a, c);
			Node* bd = fn.binary(Opcode::FMul, b, d);
			Node* bc = fn.binary(Opcode::FMul, b, c);
			Node* ad = fn.binary(Opcode::FMul, a, d);
			Node* cc = fn.binary(Opcode::FMul, c, c);
			Node* dd = fn.binary(Opcode::FMul, d, d);
			Node* denom = fn.binary(Opcode::FAdd, cc, dd);
			Node* rnum = fn.binary(Opcode::FAdd, ac, bd);
			Node* inum = fn.binary(Opcode::FSub, bc, ad);
			return makeComplex(fn, ct, fn.binary(Opcode::FDiv, rnum, denom),
												 fn.binary(Opcode::FDiv, inum, denom));
		}
		default:
			fail("invalid operator on a complex operand");
			return {};
		}
	}

	Emitter::Value Emitter::emitComplexUnary(Function& fn, ExprOp op, Value v) {
		switch (op) {
		case ExprOp::Pos:
			return v;
		case ExprOp::Neg:
			return makeComplex(fn, v.type, fn.unary(Opcode::FNeg, complexReal(fn, v)),
												 fn.unary(Opcode::FNeg, complexImag(fn, v)));
		default:
			fail("invalid operator on a complex operand");
			return {};
		}
	}
} // namespace rat::cc
