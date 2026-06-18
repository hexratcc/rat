#include "Emit.h"

#include "IR/Function.h"
#include "IR/Module.h"

namespace rat::cc {
	Emitter::Emitter(Module& module) : mod(module) {}

	void Emitter::fail(const String& msg) {
		if (failed)
			return;
		errMsg = msg;
		failed = true;
	}

	void Emitter::pushScope() { scopes.emplace_back(); }

	void Emitter::popScope() { scopes.pop_back(); }

	void Emitter::declare(const String& name, U32 var) {
		scopes.back()[name] = var;
	}

	B32 Emitter::lookup(const String& name, U32& var) const {
		for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
			auto found = it->find(name);
			if (found != it->end()) {
				var = found->second;
				return true;
			}
		}
		return false;
	}

	Node* Emitter::toBool(Function& fn, Node* value) {
		return fn.ne(value, fn.constInt(i32, 0));
	}

	Node* Emitter::fromBool(Function& fn, Node* value) {
		return fn.zext(value, i32);
	}

	Node* Emitter::emitAssign(Function& fn, const Expr* e) {
		const Expr* target = e->binary.lhs;
		if (target->kind != ExprKind::Ident) {
			fail("expression is not assignable");
			return nullptr;
		}
		U32 var;
		if (!lookup(*target->ident.name, var)) {
			fail("use of undeclared identifier '" + *target->ident.name + "'");
			return nullptr;
		}
		Node* rhs = emitExpr(fn, e->binary.rhs);
		if (!rhs)
			return nullptr;

		Node* result = nullptr;
		switch (e->binary.op) {
		case ExprOp::Assign:
			result = rhs;
			break;
		case ExprOp::AddAssign:
			result = fn.add(fn.get(var), rhs);
			break;
		case ExprOp::SubAssign:
			result = fn.sub(fn.get(var), rhs);
			break;
		case ExprOp::MulAssign:
			result = fn.mul(fn.get(var), rhs);
			break;
		case ExprOp::DivAssign:
			result = fn.sdiv(fn.get(var), rhs);
			break;
		case ExprOp::RemAssign:
			result = fn.srem(fn.get(var), rhs);
			break;
		case ExprOp::ShlAssign:
			result = fn.shl(fn.get(var), rhs);
			break;
		case ExprOp::ShrAssign:
			result = fn.ashr(fn.get(var), rhs);
			break;
		case ExprOp::AndAssign:
			result = fn.and_(fn.get(var), rhs);
			break;
		case ExprOp::OrAssign:
			result = fn.or_(fn.get(var), rhs);
			break;
		case ExprOp::XorAssign:
			result = fn.xor_(fn.get(var), rhs);
			break;
		default:
			fail("unsupported assignment operator");
			return nullptr;
		}
		fn.set(var, result);
		return result;
	}

	Node* Emitter::emitExpr(Function& fn, const Expr* e) {
		switch (e->kind) {
		case ExprKind::IntLit:
			return fn.constInt(i32, e->intLit.value);

		case ExprKind::Ident: {
			U32 var;
			if (!lookup(*e->ident.name, var)) {
				fail("use of undeclared identifier '" + *e->ident.name + "'");
				return nullptr;
			}
			return fn.get(var);
		}

		case ExprKind::Unary: {
			Node* operand = emitExpr(fn, e->unary.operand);
			if (!operand)
				return nullptr;
			switch (e->unary.op) {
			case ExprOp::Pos:
				return operand;
			case ExprOp::Neg:
				return fn.neg(operand);
			case ExprOp::BitNot:
				return fn.bitNot(operand);
			case ExprOp::Not:
				// !x  ==  (x == 0), as int 0/1
				return fromBool(fn, fn.eq(operand, fn.constInt(i32, 0)));
			default:
				fail("unsupported unary operator");
				return nullptr;
			}
		}

		case ExprKind::Binary: {
			if (isAssignOp(e->binary.op))
				return emitAssign(fn, e);
			if (e->binary.op == ExprOp::LogAnd || e->binary.op == ExprOp::LogOr) {
				Node* lhs = emitExpr(fn, e->binary.lhs);
				if (!lhs)
					return nullptr;
				Node* rhs = emitExpr(fn, e->binary.rhs);
				if (!rhs)
					return nullptr;
				Node* l = fromBool(fn, toBool(fn, lhs));
				Node* r = fromBool(fn, toBool(fn, rhs));
				return e->binary.op == ExprOp::LogAnd ? fn.and_(l, r) : fn.or_(l, r);
			}

			Node* lhs = emitExpr(fn, e->binary.lhs);
			if (!lhs)
				return nullptr;
			Node* rhs = emitExpr(fn, e->binary.rhs);
			if (!rhs)
				return nullptr;
			switch (e->binary.op) {
			case ExprOp::Add:
				return fn.add(lhs, rhs);
			case ExprOp::Sub:
				return fn.sub(lhs, rhs);
			case ExprOp::Mul:
				return fn.mul(lhs, rhs);
			case ExprOp::Div:
				return fn.sdiv(lhs, rhs);
			case ExprOp::Rem:
				return fn.srem(lhs, rhs);
			case ExprOp::Shl:
				return fn.shl(lhs, rhs);
			case ExprOp::Shr:
				return fn.ashr(lhs, rhs);
			case ExprOp::Lt:
				return fromBool(fn, fn.slt(lhs, rhs));
			case ExprOp::Gt:
				return fromBool(fn, fn.sgt(lhs, rhs));
			case ExprOp::Le:
				return fromBool(fn, fn.sle(lhs, rhs));
			case ExprOp::Ge:
				return fromBool(fn, fn.sge(lhs, rhs));
			case ExprOp::Eq:
				return fromBool(fn, fn.eq(lhs, rhs));
			case ExprOp::Ne:
				return fromBool(fn, fn.ne(lhs, rhs));
			case ExprOp::BitAnd:
				return fn.and_(lhs, rhs);
			case ExprOp::BitOr:
				return fn.or_(lhs, rhs);
			case ExprOp::BitXor:
				return fn.xor_(lhs, rhs);
			default:
				fail("unsupported binary operator");
				return nullptr;
			}
		}

		case ExprKind::Ternary: {
			Node* cond = emitExpr(fn, e->ternary.cond);
			if (!cond)
				return nullptr;
			Node* whenTrue = emitExpr(fn, e->ternary.whenTrue);
			if (!whenTrue)
				return nullptr;
			Node* whenFalse = emitExpr(fn, e->ternary.whenFalse);
			if (!whenFalse)
				return nullptr;
			Node* mask = fn.neg(fromBool(fn, toBool(fn, cond))); // 0 or -1
			Node* keepTrue = fn.and_(whenTrue, mask);
			Node* keepFalse = fn.and_(whenFalse, fn.bitNot(mask));
			return fn.or_(keepTrue, keepFalse);
		}

		case ExprKind::Comma: {
			Node* lhs = emitExpr(fn, e->comma.lhs);
			if (!lhs)
				return nullptr;
			return emitExpr(fn, e->comma.rhs);
		}
		}
		fail("unsupported expression");
		return nullptr;
	}

	B32 Emitter::emitStmt(Function& fn, const Stmt* s) {
		switch (s->kind) {
		case StmtKind::Compound: {
			pushScope();
			for (const Stmt* child : s->body) {
				if (fn.blockFinished())
					break; // unreachable code after a return
				if (!emitStmt(fn, child)) {
					popScope();
					return false;
				}
			}
			popScope();
			return true;
		}

		case StmtKind::Decl:
			for (const Declarator& d : s->decls) {
				Node* init = d.init ? emitExpr(fn, d.init) : fn.constInt(i32, 0);
				if (!init)
					return false;
				declare(*d.name, fn.declareLocal(*d.name, init));
			}
			return true;

		case StmtKind::Return: {
			Node* value =
					s->expr ? emitExpr(fn, s->expr) : fn.constInt(i32, 0);
			if (!value)
				return false;
			fn.ret(value);
			return true;
		}

		case StmtKind::Expr: {
			Node* value = emitExpr(fn, s->expr);
			return value != nullptr;
		}

		case StmtKind::Empty:
			return true;
		}
		fail("unsupported statement");
		return false;
	}

	B32 Emitter::emit(const TransUnit& unit) {
		i32 = mod.getInt(32);
		for (const FuncDef* def : unit.functions) {
			Function* fn = mod.createFunction(def->name, {}, i32);
			scopes.clear();
			if (def->body && !emitStmt(*fn, def->body))
				return false;
			if (!fn->blockFinished())
				fn->ret(fn->constInt(i32, 0)); // implicit return 0
		}
		return !failed;
	}
} // namespace rat::cc
