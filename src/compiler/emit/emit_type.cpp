#include "emit/emit.h"

namespace rat::cc {
	Symbol Emitter::lookupSymbol(const String& name) {
		Symbol sym;
		const Local* local = func.scopes.get(name);
		if(local && local->kind != Local::Kind::Function) {
			sym.kind = Symbol::Kind::Local;
			sym.local = local;
			sym.type = local->type;
			return sym;
		}
		auto global = syms.globals.find(name);
		if(!local && global != syms.globals.end()) {
			sym.kind = Symbol::Kind::Global;
			sym.type = global->second.type;
			return sym;
		}
		auto function = syms.functions.find(name);
		if(function != syms.functions.end()) {
			sym.kind = Symbol::Kind::Function;
			sym.type = funcPtrType(function->second);
		}
		return sym;
	}

	// field is null when there is no such member
	void Emitter::memberField(const Expr* e, CType& structType, const Field*& field) {
		field = nullptr;
		CType base = valueType(e->member.base->type);
		structType = e->member.arrow ? pointee(base) : base;
		if(structType.strukt)
			field = structType.strukt->find(*e->member.name);
	}

	// false when the size is not a constant
	B32 Emitter::sizeofOperand(const Expr* operand, U64& out) {
		CType t = operand->type;
		if(operand->kind == ExprKind::CompoundLit && isArrayType(t)) {
			I64 count;
			if(!compoundArrayCount(operand, count))
				return failArrayCount();
			out = (U64)count * byteSize(arrayElem(t));
			return true;
		}
		if(hasVlaDim(t))
			return false;
		out = byteSize(t);
		if(t.func && t.ptr == 0)
			out = 1; // as gcc
		return true;
	}
} // namespace rat::cc
