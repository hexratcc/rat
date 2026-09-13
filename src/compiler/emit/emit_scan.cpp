#include "emit/emit.h"

namespace rat::cc {
	namespace detail {
		B32 walkExprChildren(AstWalk& w, const Expr* e) {
			switch(e->kind) {
			case ExprKind::Unary:
				return walkExpr(w, e->unary.operand);
			case ExprKind::Binary:
				return walkExpr(w, e->binary.lhs) && walkExpr(w, e->binary.rhs);
			case ExprKind::Ternary:
				return walkExpr(w, e->ternary.cond) && walkExpr(w, e->ternary.whenTrue) &&
							 walkExpr(w, e->ternary.whenFalse);
			case ExprKind::Comma:
				return walkExpr(w, e->comma.lhs) && walkExpr(w, e->comma.rhs);
			case ExprKind::Cast:
				return walkExpr(w, e->cast.operand);
			case ExprKind::Sizeof:
				return !w.sizeofOperand || walkExpr(w, e->sizeOf.operand);
			case ExprKind::AlignOf:
				return !w.alignOfOperand || walkExpr(w, e->sizeOf.operand);
			case ExprKind::Member:
				return walkExpr(w, e->member.base);
			case ExprKind::Call:
				return walkExpr(w, e->call.target);
			case ExprKind::VaArg:
				return walkExpr(w, e->vaArg.ap);
			case ExprKind::CompoundLit:
				return !w.compoundLitInit || walkExpr(w, e->compound.init);
			case ExprKind::StmtExpr:
				return !w.stmtExprBody || walkStmt(w, e->stmtExpr.body);
			default:
				return true;
			}
		}

		// args holds the call arguments and the initializer-list elements; it is
		// empty for every other kind, so it is walked last for all of them.
		B32 walkExpr(AstWalk& w, const Expr* e) {
			if(!e)
				return true;
			if(!w.onExpr(e))
				return false;
			if(!walkExprChildren(w, e))
				return false;
			for(const Expr* a : e->args)
				if(!walkExpr(w, a))
					return false;
			return true;
		}

		B32 walkStmtChildren(AstWalk& w, const Stmt* s) {
			switch(s->kind) {
			case StmtKind::Compound:
				for(const Stmt* child : s->body)
					if(!walkStmt(w, child))
						return false;
				return true;
			case StmtKind::Decl:
				if(!w.exprChildren)
					return true;
				for(const Declarator& d : s->decls)
					if(!walkExpr(w, d.init))
						return false;
				return true;
			case StmtKind::If:
				if(w.exprChildren && !walkExpr(w, s->expr))
					return false;
				return walkStmt(w, s->thenBody) && walkStmt(w, s->elseBody);
			case StmtKind::Switch:
				if(!w.nestedSwitch)
					return true;
				if(w.exprChildren && !walkExpr(w, s->expr))
					return false;
				return walkStmt(w, s->thenBody);
			case StmtKind::While:
			case StmtKind::DoWhile:
			case StmtKind::Case:
				if(w.exprChildren && !walkExpr(w, s->expr))
					return false;
				return walkStmt(w, s->thenBody);
			case StmtKind::For:
				if(w.forInit && !walkStmt(w, s->forInit))
					return false;
				if(w.exprChildren && (!walkExpr(w, s->expr) || !walkExpr(w, s->forPost)))
					return false;
				return walkStmt(w, s->thenBody);
			case StmtKind::Label:
			case StmtKind::Default:
				return walkStmt(w, s->thenBody);
			case StmtKind::Return:
			case StmtKind::Expr:
				return !w.exprChildren || walkExpr(w, s->expr);
			default:
				return true;
			}
		}

		B32 walkStmt(AstWalk& w, const Stmt* s) {
			if(!s)
				return true;
			if(!w.onStmt(s))
				return false;
			return walkStmtChildren(w, s);
		}

		// &x, and the win64 va_list builtins, force x into memory
		struct AddrTakenWalk final : AstWalk {
			AddrTakenWalk(Set<String>& vars, B32 win64Va)
			: memVars(vars),
				win64VaList(win64Va) {
				compoundLitInit = true;
				stmtExprBody = true;
				exprChildren = true;
				nestedSwitch = true;
				forInit = true;
			}
			B32 onExpr(const Expr* e) override;
			void noteVaBuiltin(const Expr* e);
			Set<String>& memVars;
			B32 win64VaList;
		};

		void AddrTakenWalk::noteVaBuiltin(const Expr* e) {
			if(!win64VaList || !e->call.callee || e->args.empty() || e->args[0]->kind != ExprKind::Ident)
				return;
			const String& b = *e->call.callee;
			if(b == "__builtin_va_start" || b == "__builtin_va_end" || b == "__builtin_va_copy")
				memVars.insert(*e->args[0]->ident.name);
		}

		B32 AddrTakenWalk::onExpr(const Expr* e) {
			switch(e->kind) {
			case ExprKind::Unary:
				if(e->unary.op == ExprOp::Addr && e->unary.operand->kind == ExprKind::Ident)
					memVars.insert(*e->unary.operand->ident.name);
				return true;
			case ExprKind::Call:
				noteVaBuiltin(e);
				return true;
			case ExprKind::VaArg:
				if(win64VaList && e->vaArg.ap->kind == ExprKind::Ident)
					memVars.insert(*e->vaArg.ap->ident.name);
				return true;
			default:
				return true;
			}
		}

		struct LabelWalkBase : AstWalk {
			LabelWalkBase() {
				sizeofOperand = true;
				alignOfOperand = true;
				stmtExprBody = true;
				exprChildren = true;
				nestedSwitch = true;
				forInit = true;
			}
		};

		struct LabelBlockWalk final : LabelWalkBase {
			LabelBlockWalk(Function& func, Map<String, Function::Block*>& blocks)
			: fn(func),
				labelBlocks(blocks) {}
			B32 onStmt(const Stmt* s) override;
			Function& fn;
			Map<String, Function::Block*>& labelBlocks;
		};

		B32 LabelBlockWalk::onStmt(const Stmt* s) {
			if(s->kind == StmtKind::Label && !labelBlocks.count(*s->label))
				labelBlocks[*s->label] = fn.createLoopHeader("label." + *s->label);
			return true;
		}

		struct HasLabelWalk final : LabelWalkBase {
			B32 onStmt(const Stmt* s) override { return s->kind != StmtKind::Label; }
		};

		struct RefersToWalk final : AstWalk {
			explicit RefersToWalk(const String& n)
			: name(n) {
				sizeofOperand = true;
				compoundLitInit = true;
			}
			B32 onExpr(const Expr* e) override;
			const String& name;
		};

		B32 RefersToWalk::onExpr(const Expr* e) {
			if(e->kind != ExprKind::Ident)
				return true;
			return !e->ident.name || *e->ident.name != name;
		}

		struct HasSwitchCaseWalk final : AstWalk {
			B32 onStmt(const Stmt* s) override {
				return s->kind != StmtKind::Case && s->kind != StmtKind::Default;
			}
		};

		struct SwitchCaseWalk final : AstWalk {
			SwitchCaseWalk(List<const Stmt*>& list, const Stmt*& defStmt)
			: cases(list),
				def(defStmt) {}
			B32 onStmt(const Stmt* s) override;
			List<const Stmt*>& cases;
			const Stmt*& def;
		};

		B32 SwitchCaseWalk::onStmt(const Stmt* s) {
			if(s->kind == StmtKind::Case)
				cases.push_back(s);
			else if(s->kind == StmtKind::Default)
				def = s;
			return true;
		}
	} // namespace detail

	void Emitter::collectAddrTaken(const Stmt* s) {
		detail::AddrTakenWalk w(memVars, lay.win64VaList);
		detail::walkStmt(w, s);
	}

	void Emitter::collectLabels(Function& fn, const Stmt* s) {
		detail::LabelBlockWalk w(fn, labelBlocks);
		detail::walkStmt(w, s);
	}

	B32 Emitter::containsLabel(const Stmt* s) {
		detail::HasLabelWalk w;
		return !detail::walkStmt(w, s);
	}

	B32 Emitter::containsSwitchCase(const Stmt* s) {
		detail::HasSwitchCaseWalk w;
		return !detail::walkStmt(w, s);
	}

	void Emitter::collectSwitchCases(const Stmt* s, List<const Stmt*>& cases, const Stmt*& def) {
		detail::SwitchCaseWalk w(cases, def);
		detail::walkStmt(w, s);
	}

	B32 Emitter::exprRefersTo(const Expr* e, const String& name) const {
		detail::RefersToWalk w(name);
		return !detail::walkExpr(w, e);
	}
} // namespace rat::cc
