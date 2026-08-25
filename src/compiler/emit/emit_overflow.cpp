#include "emit/emit.h"

namespace rat::cc {
	namespace {
		struct OvfForm {
			Opcode op = Opcode::Add;
			B32 predicate = false;
			B32 typed = false;
			CType fixed;
		};

		B32 stemOp(const String& s, U64 pos, Opcode& op) {
			if(s.compare(pos, 3, "add") == 0)
				op = Opcode::Add;
			else if(s.compare(pos, 3, "sub") == 0)
				op = Opcode::Sub;
			else if(s.compare(pos, 3, "mul") == 0)
				op = Opcode::Mul;
			else
				return false;
			return true;
		}

		B32 parseOverflowName(const String& name, U32 longBits, OvfForm& out) {
			if(name.rfind("__builtin_", 0) != 0)
				return false;
			String stem = name.substr(10);
			if(stem.size() > 11 && stem.compare(stem.size() - 11, 11, "_overflow_p") == 0) {
				out.predicate = true;
				stem.resize(stem.size() - 11);
			} else if(stem.size() > 9 && stem.compare(stem.size() - 9, 9, "_overflow") == 0) {
				stem.resize(stem.size() - 9);
			} else {
				return false;
			}
			if(stem.size() == 3)
				return stemOp(stem, 0, out.op);
			if(out.predicate || stem.size() < 4)
				return false;
			B32 uns = stem[0] == 'u';
			if(!uns && stem[0] != 's')
				return false;
			if(!stemOp(stem, 1, out.op))
				return false;
			CType t;
			t.set(CType::Unsigned, uns);
			String w = stem.substr(4);
			if(w.empty()) {
				t.bits = 32;
			} else if(w == "l") {
				t.bits = longBits;
				t.set(CType::Long);
			} else if(w == "ll") {
				t.bits = 64;
				t.set(CType::LongLong);
			} else {
				return false;
			}
			out.typed = true;
			out.fixed = t;
			return true;
		}

		U32 signedWidth(CType t) { return t.bits + (t.isUnsigned() ? 1u : 0u); }

		B32 exactInSigned64(Opcode op, CType a, CType b) {
			U32 sa = signedWidth(a);
			U32 sb = signedWidth(b);
			if(op == Opcode::Mul)
				return sa + sb <= 65;
			U32 m = sa > sb ? sa : sb;
			return m + 1 <= 64;
		}

		B32 exactInUnsigned64(Opcode op, CType a, CType b) {
			if(op == Opcode::Sub || !a.isUnsigned() || !b.isUnsigned())
				return false;
			if(op == Opcode::Mul)
				return a.bits + b.bits <= 64;
			U32 m = a.bits > b.bits ? a.bits : b.bits;
			return m + 1 <= 64;
		}

		CType ctWide(B32 uns) {
			CType t;
			t.bits = 64;
			t.set(CType::Unsigned, uns);
			return t;
		}
	} // namespace

	Emitter::Wide Emitter::wideExtend(Function& fn, Node* v, CType from) {
		Wide r;
		r.lo = convert(fn, v, from, ctWide(from.isUnsigned()));
		if(from.isUnsigned())
			r.hi = fn.constInt(r.lo->getType(), 0);
		else
			r.hi = fn.ashr(r.lo, fn.constInt(i32, 63));
		return r;
	}

	Emitter::Wide Emitter::wideAddSub(Function& fn, Wide a, Wide b, B32 sub) {
		Type* t = a.lo->getType();
		Wide r;
		if(sub) {
			r.lo = fn.sub(a.lo, b.lo);
			Node* borrow = fn.zext(fn.compare(Opcode::Ult, a.lo, b.lo), t);
			r.hi = fn.sub(fn.sub(a.hi, b.hi), borrow);
		} else {
			r.lo = fn.add(a.lo, b.lo);
			Node* carry = fn.zext(fn.compare(Opcode::Ult, r.lo, a.lo), t);
			r.hi = fn.add(fn.add(a.hi, b.hi), carry);
		}
		return r;
	}

	Emitter::Wide Emitter::wideMul(Function& fn, Wide a, Wide b) {
		Type* t = a.lo->getType();
		Node* sh = fn.constInt(i32, 32);
		Node* mask = fn.constInt(t, (I64)0xffffffffull);
		Node* a0 = fn.and_(a.lo, mask);
		Node* a1 = fn.lshr(a.lo, sh);
		Node* b0 = fn.and_(b.lo, mask);
		Node* b1 = fn.lshr(b.lo, sh);
		Node* p00 = fn.mul(a0, b0);
		Node* p01 = fn.mul(a0, b1);
		Node* p10 = fn.mul(a1, b0);
		Node* p11 = fn.mul(a1, b1);
		Node* mid = fn.add(p01, p10);
		Node* midCarry = fn.zext(fn.compare(Opcode::Ult, mid, p01), t);
		Wide r;
		r.lo = fn.add(p00, fn.shl(mid, sh));
		Node* loCarry = fn.zext(fn.compare(Opcode::Ult, r.lo, p00), t);
		r.hi = fn.add(p11, fn.lshr(mid, sh));
		r.hi = fn.add(r.hi, fn.shl(midCarry, sh));
		r.hi = fn.add(r.hi, loCarry);
		r.hi = fn.sub(r.hi, fn.and_(a.hi, b.lo));
		r.hi = fn.sub(r.hi, fn.and_(b.hi, a.lo));
		return r;
	}

	Node* Emitter::wideFits(Function& fn, Wide v, CType rt) {
		Type* t = v.lo->getType();
		U32 n = rt.bits;
		Node* hiOk;
		Node* loOk = nullptr;
		if(rt.isUnsigned()) {
			hiOk = fn.eq(v.hi, fn.constInt(t, 0));
			if(n < 64)
				loOk = fn.compare(Opcode::Ule, v.lo, fn.constInt(t, (I64)((1ull << n) - 1)));
		} else {
			hiOk = fn.eq(v.hi, fn.ashr(v.lo, fn.constInt(i32, 63)));
			if(n < 64) {
				Node* biased = fn.add(v.lo, fn.constInt(t, (I64)(1ull << (n - 1))));
				loOk = fn.compare(Opcode::Ule, biased, fn.constInt(t, (I64)((1ull << n) - 1)));
			}
		}
		if(!loOk)
			return hiOk;
		return fn.and_(hiOk, loOk);
	}

	Node* Emitter::fitsIn64(Function& fn, Node* r, CType rt, B32 sgn) {
		Type* t = r->getType();
		U32 n = rt.bits;
		if(rt.isUnsigned()) {
			if(n < 64)
				return fn.compare(Opcode::Ule, r, fn.constInt(t, (I64)((1ull << n) - 1)));
			if(!sgn)
				return fn.constBool(true);
			return fn.compare(Opcode::Sle, fn.constInt(t, 0), r);
		}
		if(!sgn)
			return fn.compare(Opcode::Ule, r, fn.constInt(t, (I64)((1ull << (n - 1)) - 1)));
		if(n >= 64)
			return fn.constBool(true);
		Node* biased = fn.add(r, fn.constInt(t, (I64)(1ull << (n - 1))));
		return fn.compare(Opcode::Ule, biased, fn.constInt(t, (I64)((1ull << n) - 1)));
	}

	B32 Emitter::emitOverflowBuiltin(Function& fn, const Expr* e, Value& out) {
		const String& b = *e->call.callee;
		OvfForm f;
		if(!parseOverflowName(b, lay.longBits, f))
			return false;
		if(e->args.size() != 3) {
			fail("'" + b + "' expects three arguments");
			return true;
		}
		Value a0 = emitExpr(fn, e->args[0]);
		Value a1 = emitExpr(fn, e->args[1]);
		Value a2 = emitExpr(fn, e->args[2]);
		if(!a0.node || !a1.node || !a2.node)
			return true;
		if(!f.predicate && !isPointer(a2.type)) {
			fail("the last argument of '" + b + "' must be a pointer to an integer");
			return true;
		}

		CType at;
		CType bt;
		CType rt;
		Node* an;
		Node* bn;
		if(f.typed) {
			at = f.fixed;
			bt = f.fixed;
			rt = f.fixed;
			an = convert(fn, a0.node, a0.type, at);
			bn = convert(fn, a1.node, a1.type, bt);
		} else {
			at = a0.type;
			bt = a1.type;
			an = a0.node;
			bn = a1.node;
			rt = a2.type;
			if(!f.predicate)
				--rt.ptr;
		}
		if(!isInteger(at) || !isInteger(bt) || !isInteger(rt)) {
			fail("'" + b + "' works on integer types only");
			return true;
		}

		Node* fits;
		Node* value;
		B32 sgn = exactInSigned64(f.op, at, bt);
		if(sgn || exactInUnsigned64(f.op, at, bt)) {
			CType w = ctWide(!sgn);
			Node* r = fn.binary(f.op, convert(fn, an, at, w), convert(fn, bn, bt, w));
			fits = fitsIn64(fn, r, rt, sgn);
			value = convert(fn, r, w, rt);
		} else {
			Wide wa = wideExtend(fn, an, at);
			Wide wb = wideExtend(fn, bn, bt);
			Wide r;
			if(f.op == Opcode::Mul)
				r = wideMul(fn, wa, wb);
			else
				r = wideAddSub(fn, wa, wb, f.op == Opcode::Sub);
			fits = wideFits(fn, r, rt);
			value = convert(fn, r.lo, ctWide(true), rt);
		}
		if(!f.predicate)
			fn.store(a2.node, value);
		out = {fromBool(fn, fn.eq(fits, fn.constBool(false))), ctInt()};
		return true;
	}
} // namespace rat::cc
