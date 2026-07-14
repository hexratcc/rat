#include "Emit/Emit.h"

namespace rat::cc {
	void Emitter::collectAddrTakenExpr(const Expr* e) {
		if(!e)
			return;
		switch(e->kind) {
		case ExprKind::Unary:
			if(e->unary.op == ExprOp::Addr && e->unary.operand->kind == ExprKind::Ident)
				memVars.insert(*e->unary.operand->ident.name);
			collectAddrTakenExpr(e->unary.operand);
			return;
		case ExprKind::Binary:
			collectAddrTakenExpr(e->binary.lhs);
			collectAddrTakenExpr(e->binary.rhs);
			return;
		case ExprKind::Ternary:
			collectAddrTakenExpr(e->ternary.cond);
			collectAddrTakenExpr(e->ternary.whenTrue);
			collectAddrTakenExpr(e->ternary.whenFalse);
			return;
		case ExprKind::Comma:
			collectAddrTakenExpr(e->comma.lhs);
			collectAddrTakenExpr(e->comma.rhs);
			return;
		case ExprKind::Cast:
			collectAddrTakenExpr(e->cast.operand);
			return;
		case ExprKind::Call:
			collectAddrTakenExpr(e->call.target);
			for(const Expr* arg : e->args)
				collectAddrTakenExpr(arg);
			return;
		case ExprKind::Member:
			collectAddrTakenExpr(e->member.base);
			return;
		case ExprKind::InitList:
			for(const Expr* el : e->args)
				collectAddrTakenExpr(el);
			return;
		case ExprKind::CompoundLit:
			collectAddrTakenExpr(e->compound.init);
			return;
		case ExprKind::StmtExpr:
			collectAddrTaken(e->stmtExpr.body);
			return;
		default:
			return;
		}
	}

	void Emitter::collectAddrTaken(const Stmt* s) {
		if(!s)
			return;
		switch(s->kind) {
		case StmtKind::Compound:
			for(const Stmt* child : s->body)
				collectAddrTaken(child);
			return;
		case StmtKind::Decl:
			for(const Declarator& d : s->decls)
				collectAddrTakenExpr(d.init);
			return;
		case StmtKind::If:
			collectAddrTakenExpr(s->expr);
			collectAddrTaken(s->thenBody);
			collectAddrTaken(s->elseBody);
			return;
		case StmtKind::While:
		case StmtKind::DoWhile:
		case StmtKind::Switch:
			collectAddrTakenExpr(s->expr);
			collectAddrTaken(s->thenBody);
			return;
		case StmtKind::For:
			collectAddrTaken(s->forInit);
			collectAddrTakenExpr(s->expr);
			collectAddrTakenExpr(s->forPost);
			collectAddrTaken(s->thenBody);
			return;
		case StmtKind::Label:
			collectAddrTaken(s->thenBody);
			return;
		case StmtKind::Case:
		case StmtKind::Return:
		case StmtKind::Expr:
			collectAddrTakenExpr(s->expr);
			return;
		default:
			return;
		}
	}

	void Emitter::collectLabelsInExpr(Function& fn, const Expr* e) {
		if(!e)
			return;
		switch(e->kind) {
		case ExprKind::StmtExpr:
			collectLabels(fn, e->stmtExpr.body);
			break;
		case ExprKind::Unary:
			collectLabelsInExpr(fn, e->unary.operand);
			break;
		case ExprKind::Binary:
			collectLabelsInExpr(fn, e->binary.lhs);
			collectLabelsInExpr(fn, e->binary.rhs);
			break;
		case ExprKind::Ternary:
			collectLabelsInExpr(fn, e->ternary.cond);
			collectLabelsInExpr(fn, e->ternary.whenTrue);
			collectLabelsInExpr(fn, e->ternary.whenFalse);
			break;
		case ExprKind::Comma:
			collectLabelsInExpr(fn, e->comma.lhs);
			collectLabelsInExpr(fn, e->comma.rhs);
			break;
		case ExprKind::Cast:
			collectLabelsInExpr(fn, e->cast.operand);
			break;
		case ExprKind::Sizeof:
			collectLabelsInExpr(fn, e->sizeOf.operand);
			break;
		case ExprKind::Member:
			collectLabelsInExpr(fn, e->member.base);
			break;
		case ExprKind::Call:
			collectLabelsInExpr(fn, e->call.target);
			break;
		case ExprKind::VaArg:
			collectLabelsInExpr(fn, e->vaArg.ap);
			break;
		default:
			break;
		}
		for(const Expr* a : e->args)
			collectLabelsInExpr(fn, a);
	}

	void Emitter::collectLabels(Function& fn, const Stmt* s) {
		if(!s)
			return;
		switch(s->kind) {
		case StmtKind::Label:
			if(!labelBlocks.count(*s->label))
				labelBlocks[*s->label] = fn.createLoopHeader("label." + *s->label);
			collectLabels(fn, s->thenBody);
			return;
		case StmtKind::Compound:
			for(const Stmt* child : s->body)
				collectLabels(fn, child);
			return;
		case StmtKind::If:
			collectLabelsInExpr(fn, s->expr);
			collectLabels(fn, s->thenBody);
			collectLabels(fn, s->elseBody);
			return;
		case StmtKind::While:
		case StmtKind::DoWhile:
		case StmtKind::Switch:
			collectLabelsInExpr(fn, s->expr);
			collectLabels(fn, s->thenBody);
			return;
		case StmtKind::For:
			collectLabels(fn, s->forInit);
			collectLabelsInExpr(fn, s->expr);
			collectLabelsInExpr(fn, s->forPost);
			collectLabels(fn, s->thenBody);
			return;
		case StmtKind::Expr:
		case StmtKind::Return:
		case StmtKind::Case:
			collectLabelsInExpr(fn, s->expr);
			return;
		case StmtKind::Decl:
			for(const Declarator& d : s->decls)
				collectLabelsInExpr(fn, d.init);
			return;
		default:
			return;
		}
	}

	B32 Emitter::containsLabelInExpr(const Expr* e) {
		if(!e)
			return false;
		switch(e->kind) {
		case ExprKind::StmtExpr:
			if(containsLabel(e->stmtExpr.body))
				return true;
			break;
		case ExprKind::Unary:
			if(containsLabelInExpr(e->unary.operand))
				return true;
			break;
		case ExprKind::Binary:
			if(containsLabelInExpr(e->binary.lhs) || containsLabelInExpr(e->binary.rhs))
				return true;
			break;
		case ExprKind::Ternary:
			if(containsLabelInExpr(e->ternary.cond) || containsLabelInExpr(e->ternary.whenTrue) ||
				 containsLabelInExpr(e->ternary.whenFalse))
				return true;
			break;
		case ExprKind::Comma:
			if(containsLabelInExpr(e->comma.lhs) || containsLabelInExpr(e->comma.rhs))
				return true;
			break;
		case ExprKind::Cast:
			if(containsLabelInExpr(e->cast.operand))
				return true;
			break;
		case ExprKind::Sizeof:
			if(containsLabelInExpr(e->sizeOf.operand))
				return true;
			break;
		case ExprKind::Member:
			if(containsLabelInExpr(e->member.base))
				return true;
			break;
		case ExprKind::Call:
			if(containsLabelInExpr(e->call.target))
				return true;
			break;
		case ExprKind::VaArg:
			if(containsLabelInExpr(e->vaArg.ap))
				return true;
			break;
		default:
			break;
		}
		for(const Expr* a : e->args)
			if(containsLabelInExpr(a))
				return true;
		return false;
	}

	B32 Emitter::containsLabel(const Stmt* s) {
		if(!s)
			return false;
		switch(s->kind) {
		case StmtKind::Label:
			return true;
		case StmtKind::Compound:
			for(const Stmt* child : s->body)
				if(containsLabel(child))
					return true;
			return false;
		case StmtKind::If:
			return containsLabelInExpr(s->expr) || containsLabel(s->thenBody) ||
						 containsLabel(s->elseBody);
		case StmtKind::While:
		case StmtKind::DoWhile:
		case StmtKind::Switch:
			return containsLabelInExpr(s->expr) || containsLabel(s->thenBody);
		case StmtKind::For:
			return containsLabel(s->forInit) || containsLabelInExpr(s->expr) ||
						 containsLabelInExpr(s->forPost) || containsLabel(s->thenBody);
		case StmtKind::Expr:
		case StmtKind::Return:
		case StmtKind::Case:
			return containsLabelInExpr(s->expr);
		case StmtKind::Decl:
			for(const Declarator& d : s->decls)
				if(containsLabelInExpr(d.init))
					return true;
			return false;
		default:
			return false;
		}
	}

	B32 Emitter::containsSwitchCase(const Stmt* s) {
		if(!s)
			return false;
		switch(s->kind) {
		case StmtKind::Case:
		case StmtKind::Default:
			return true;
		case StmtKind::Label:
			return containsSwitchCase(s->thenBody);
		case StmtKind::Compound:
			for(const Stmt* child : s->body)
				if(containsSwitchCase(child))
					return true;
			return false;
		case StmtKind::If:
			return containsSwitchCase(s->thenBody) || containsSwitchCase(s->elseBody);
		case StmtKind::While:
		case StmtKind::DoWhile:
			return containsSwitchCase(s->thenBody);
		case StmtKind::For:
			return containsSwitchCase(s->thenBody);
		default:
			return false;
		}
	}
} // namespace rat::cc
