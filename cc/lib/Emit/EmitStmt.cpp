#include "Emit/Emit.h"

namespace rat::cc {
	Node* Emitter::emitCondPred(Function& fn, const Expr* cond) {
		Value v = emitExpr(fn, cond);
		if(!v.node)
			return nullptr;
		return toBool(fn, v);
	}

	B32 Emitter::emitCondBranch(Function& fn,
															const Expr* e,
															Function::Block* trueB,
															Function::Block* falseB) {
		if(e->kind == ExprKind::Binary
			 && (e->binary.op == ExprOp::LogAnd || e->binary.op == ExprOp::LogOr)) {
			B32 isAnd = e->binary.op == ExprOp::LogAnd;
			Function::Block* rhsB = fn.createBlock(isAnd ? "and.rhs" : "or.rhs");
			if(!emitCondBranch(fn, e->binary.lhs, isAnd ? rhsB : trueB, isAnd ? falseB : rhsB))
				return false;
			fn.seal(rhsB);
			fn.setInsertBlock(rhsB);
			return emitCondBranch(fn, e->binary.rhs, trueB, falseB);
		}
		if(e->kind == ExprKind::Unary && e->unary.op == ExprOp::Not)
			return emitCondBranch(fn, e->unary.operand, falseB, trueB);
		Node* pred = emitCondPred(fn, e);
		if(!pred)
			return false;
		fn.jumpif(pred, trueB);
		fn.jmp(falseB);
		return true;
	}

	B32 Emitter::emitIf(Function& fn, const Stmt* s) {
		Function::Block* thenB = fn.createBlock("if.then");
		Function::Block* elseB = s->elseBody ? fn.createBlock("if.else") : nullptr;
		Function::Block* endB = fn.createBlock("if.end");
		B32 reaches = false;

		if(!emitCondBranch(fn, s->expr, thenB, elseB ? elseB : endB))
			return false;
		if(!elseB)
			reaches = true;

		fn.seal(thenB);
		fn.setInsertBlock(thenB);
		if(!emitStmt(fn, s->thenBody))
			return false;
		if(!fn.blockFinished()) {
			fn.jmp(endB);
			reaches = true;
		}

		if(elseB) {
			fn.seal(elseB);
			fn.setInsertBlock(elseB);
			if(!emitStmt(fn, s->elseBody))
				return false;
			if(!fn.blockFinished()) {
				fn.jmp(endB);
				reaches = true;
			}
		}

		fn.seal(endB);
		if(reaches)
			fn.setInsertBlock(endB);
		return true;
	}

	B32 Emitter::emitWhile(Function& fn, const Stmt* s) {
		Function::Block* header = fn.createLoopHeader("while.header");
		Function::Block* bodyB = fn.createBlock("while.body");
		Function::Block* exitB = fn.createBlock("while.exit");

		fn.jmp(header);
		fn.setInsertBlock(header);
		if(!emitCondBranch(fn, s->expr, bodyB, exitB))
			return false;

		fn.seal(bodyB);
		fn.setInsertBlock(bodyB);
		loops.push_back({exitB, header, true});
		B32 ok = emitStmt(fn, s->thenBody);
		loops.pop_back();
		if(!ok)
			return false;
		if(!fn.blockFinished())
			fn.jmp(header);

		fn.seal(header);
		fn.seal(exitB);
		fn.setInsertBlock(exitB);
		return true;
	}

	B32 Emitter::emitDoWhile(Function& fn, const Stmt* s) {
		Function::Block* bodyB = fn.createLoopHeader("do.body");
		Function::Block* condB = fn.createBlock("do.cond");
		Function::Block* exitB = fn.createBlock("do.exit");

		fn.jmp(bodyB);
		fn.setInsertBlock(bodyB);
		loops.push_back({exitB, condB, true});
		B32 ok = emitStmt(fn, s->thenBody);
		loops.pop_back();
		if(!ok)
			return false;
		if(!fn.blockFinished())
			fn.jmp(condB);

		fn.seal(condB);
		fn.setInsertBlock(condB);
		if(!emitCondBranch(fn, s->expr, bodyB, exitB))
			return false;

		fn.seal(bodyB);
		fn.seal(exitB);
		fn.setInsertBlock(exitB);
		return true;
	}

	B32 Emitter::emitFor(Function& fn, const Stmt* s) {
		pushScope();
		if(s->forInit && !emitStmt(fn, s->forInit)) {
			popScope();
			return false;
		}

		Function::Block* header = fn.createLoopHeader("for.header");
		Function::Block* bodyB = fn.createBlock("for.body");
		Function::Block* postB = fn.createBlock("for.post");
		Function::Block* exitB = fn.createBlock("for.exit");

		fn.jmp(header);
		fn.setInsertBlock(header);
		B32 exitReachable = false;
		if(s->expr) {
			if(!emitCondBranch(fn, s->expr, bodyB, exitB)) {
				popScope();
				return false;
			}
			exitReachable = true;
		} else {
			fn.jmp(bodyB);
		}

		fn.seal(bodyB);
		fn.setInsertBlock(bodyB);
		loops.push_back({exitB, postB, exitReachable});
		B32 ok = emitStmt(fn, s->thenBody);
		LoopFrame frame = loops.back();
		loops.pop_back();
		if(!ok) {
			popScope();
			return false;
		}
		if(!fn.blockFinished())
			fn.jmp(postB);

		fn.seal(postB);
		fn.setInsertBlock(postB);
		if(s->forPost) {
			Value post = emitExpr(fn, s->forPost);
			if(!post.node) {
				popScope();
				return false;
			}
		}
		if(!fn.blockFinished())
			fn.jmp(header);

		fn.seal(header);
		fn.seal(exitB);
		if(frame.exitReachable)
			fn.setInsertBlock(exitB);
		popScope();
		return true;
	}

	B32 Emitter::exprRefersTo(const Expr* e, const String& name) const {
		if(!e)
			return false;
		switch(e->kind) {
		case ExprKind::Ident:
			return e->ident.name && *e->ident.name == name;
		case ExprKind::Unary:
			return exprRefersTo(e->unary.operand, name);
		case ExprKind::Binary:
			return exprRefersTo(e->binary.lhs, name) || exprRefersTo(e->binary.rhs, name);
		case ExprKind::Ternary:
			return exprRefersTo(e->ternary.cond, name) || exprRefersTo(e->ternary.whenTrue, name) ||
						 exprRefersTo(e->ternary.whenFalse, name);
		case ExprKind::Comma:
			return exprRefersTo(e->comma.lhs, name) || exprRefersTo(e->comma.rhs, name);
		case ExprKind::Cast:
			return exprRefersTo(e->cast.operand, name);
		case ExprKind::Sizeof:
			return exprRefersTo(e->sizeOf.operand, name);
		case ExprKind::Member:
			return exprRefersTo(e->member.base, name);
		case ExprKind::VaArg:
			return exprRefersTo(e->vaArg.ap, name);
		case ExprKind::CompoundLit:
			if(exprRefersTo(e->compound.init, name))
				return true;
			break;
		case ExprKind::Call:
			if(exprRefersTo(e->call.target, name))
				return true;
			break;
		default:
			break;
		}
		for(const Expr* a : e->args)
			if(exprRefersTo(a, name))
				return true;
		return false;
	}

	void Emitter::collectSwitchCases(const Stmt* s, List<const Stmt*>& cases, const Stmt*& def) {
		if(!s)
			return;
		switch(s->kind) {
		case StmtKind::Case:
			cases.push_back(s);
			return;
		case StmtKind::Default:
			def = s;
			return;
		case StmtKind::Switch:
			return;
		case StmtKind::Compound:
			for(const Stmt* c : s->body)
				collectSwitchCases(c, cases, def);
			return;
		case StmtKind::If:
			collectSwitchCases(s->thenBody, cases, def);
			collectSwitchCases(s->elseBody, cases, def);
			return;
		case StmtKind::While:
		case StmtKind::DoWhile:
		case StmtKind::For:
		case StmtKind::Label:
			collectSwitchCases(s->thenBody, cases, def);
			return;
		default:
			return;
		}
	}

	B32 Emitter::emitSwitch(Function& fn, const Stmt* s) {
		Value ctrl = emitExpr(fn, s->expr);
		if(!ctrl.node)
			return false;
		if(!isInteger(ctrl.type)) {
			fail("switch controlling expression must have integer type");
			return false;
		}
		CType ct = promote(ctrl.type);
		Node* val = convert(fn, ctrl.node, ctrl.type, ct);

		const Stmt* body = s->thenBody;
		if(body->kind != StmtKind::Compound) {
			fail("switch body must be a block");
			return false;
		}

		Function::Block* exitB = fn.createBlock("switch.exit");
		List<const Stmt*> caseStmts;
		const Stmt* defaultStmt = nullptr;
		collectSwitchCases(body, caseStmts, defaultStmt);

		Map<const Stmt*, Function::Block*> blocks;
		List<I64> caseValues;
		List<Function::Block*> caseBlocks;
		for(const Stmt* c : caseStmts) {
			I64 v;
			if(!evalConst(c->expr, v)) {
				fail("case label is not an integer constant expression");
				return false;
			}
			for(I64 prev : caseValues) {
				if(prev == v) {
					fail("duplicate case value in switch");
					return false;
				}
			}
			Function::Block* b = fn.createBlock("switch.case");
			blocks[c] = b;
			caseValues.push_back(v);
			caseBlocks.push_back(b);
		}
		Function::Block* defaultBlock = nullptr;
		if(defaultStmt) {
			defaultBlock = fn.createBlock("switch.default");
			blocks[defaultStmt] = defaultBlock;
		}

		// dispatch
		Function::Block* missB = defaultBlock ? defaultBlock : exitB;
		List<U32> order(caseValues.size());
		for(U32 i = 0; i < order.size(); ++i)
			order[i] = i;
		B32 uns = ct.isUnsigned();
		std::sort(order.begin(), order.end(), [&](U32 a, U32 b) {
			if(uns)
				return (U64)caseValues[a] < (U64)caseValues[b];
			return caseValues[a] < caseValues[b];
		});
		constexpr U32 kLinearMax = 4;
		auto emitRange = [&](auto&& self, U32 lo, U32 hi) -> void {
			if(hi - lo <= kLinearMax) {
				for(U32 i = lo; i < hi; ++i) {
					Node* c = fn.eq(val, fn.constInt(irType(ct), caseValues[order[i]]));
					fn.jumpif(c, caseBlocks[order[i]]);
				}
				fn.jmp(missB);
				return;
			}
			U32 mid = lo + (hi - lo) / 2;
			Function::Block* ltB = fn.createBlock("switch.lt");
			Node* pivot = fn.constInt(irType(ct), caseValues[order[mid]]);
			fn.jumpif(fn.compare(uns ? Opcode::Ult : Opcode::Slt, val, pivot), ltB);
			self(self, mid, hi); // fallthrough side: val >= pivot
			fn.seal(ltB);
			fn.setInsertBlock(ltB);
			self(self, lo, mid);
		};
		emitRange(emitRange, 0, (U32)order.size());
		switches.push_back(std::move(blocks));
		loops.push_back({exitB, nullptr, false, true});
		B32 ok = emitStmt(fn, body);
		LoopFrame frame = loops.back();
		loops.pop_back();
		switches.pop_back();
		if(!ok)
			return false;

		if(!fn.blockFinished()) {
			fn.jmp(exitB);
			frame.exitReachable = true;
		}
		if(!defaultBlock)
			frame.exitReachable = true;

		fn.seal(exitB);
		if(frame.exitReachable)
			fn.setInsertBlock(exitB);
		return true;
	}

	B32 Emitter::emitCompound(Function& fn, const Stmt* s) {
		pushScope();
		for(const Stmt* child : s->body) {
			if(fn.blockFinished() && child->kind != StmtKind::Label && child->kind != StmtKind::Case &&
				 child->kind != StmtKind::Default && !containsLabel(child) &&
				 !(!switches.empty() && containsSwitchCase(child))) {
				if(child->kind == StmtKind::Decl && !declareDead(fn, child)) {
					popScope();
					return false;
				}
				continue;
			}
			if(fn.blockFinished() && child->kind != StmtKind::Label && child->kind != StmtKind::Case &&
				 child->kind != StmtKind::Default) {
				Function::Block* dead = fn.createBlock("dead");
				fn.seal(dead);
				fn.setInsertBlock(dead);
			}
			if(!emitStmt(fn, child)) {
				popScope();
				return false;
			}
		}
		popScope();
		return true;
	}

	B32 Emitter::emitCaseLabel(Function& fn, const Stmt* s) {
		if(switches.empty()) {
			fail("'case'/'default' label not within a switch");
			return false;
		}
		auto it = switches.back().find(s);
		if(it == switches.back().end()) {
			fail("'case'/'default' label not within a switch");
			return false;
		}
		Function::Block* lbl = it->second;
		if(!fn.blockFinished())
			fn.jmp(lbl);
		fn.seal(lbl);
		fn.setInsertBlock(lbl);
		return true;
	}

	B32 Emitter::emitStmt(Function& fn, const Stmt* s) {
		curOffset = s->offset;
		switch(s->kind) {
		case StmtKind::Compound:
			return emitCompound(fn, s);
		case StmtKind::Decl:
			return emitDecl(fn, s);
		case StmtKind::If:
			return emitIf(fn, s);
		case StmtKind::While:
			return emitWhile(fn, s);
		case StmtKind::DoWhile:
			return emitDoWhile(fn, s);
		case StmtKind::For:
			return emitFor(fn, s);
		case StmtKind::Switch:
			return emitSwitch(fn, s);
		case StmtKind::Case:
		case StmtKind::Default:
			return emitCaseLabel(fn, s);
		case StmtKind::Break:
			if(loops.empty()) {
				fail("'break' statement not in a loop or switch");
				return false;
			}
			loops.back().exitReachable = true;
			fn.jmp(loops.back().brk);
			return true;
		case StmtKind::Continue: {
			for(auto it = loops.rbegin(); it != loops.rend(); ++it) {
				if(it->isSwitch)
					continue;
				fn.jmp(it->cont);
				return true;
			}
			fail("'continue' statement not in a loop");
			return false;
		}
		case StmtKind::Return:
			return emitReturn(fn, s);
		case StmtKind::Expr:
			return emitExprStmt(fn, s);
		case StmtKind::Empty:
			return true;
		case StmtKind::Label:
			return emitLabel(fn, s);
		case StmtKind::Goto:
			return emitGoto(fn, s);
		}
		fail("unsupported statement");
		return false;
	}

	B32 Emitter::emitReturn(Function& fn, const Stmt* s) {
		if(sretSlot) {
			if(s->expr) {
				Value v = emitExpr(fn, s->expr);
				if(!v.node)
					return false;
				if(isComplexType(curRet)) {
					storeComplex(fn, sretSlot, completeComplex(curRet), v);
				} else if(!isStruct(v.type) || v.type.strukt != curRet.strukt) {
					fail("invalid return value for a struct/union function");
					return false;
				} else {
					emitMemCopy(fn, sretSlot, v.node, curRet.strukt->size);
				}
			}
			fn.ret(sretSlot);
			return true;
		}
		if(isVoidType(curRet)) {
			if(s->expr) {
				Value v = emitExpr(fn, s->expr);
				if(!v.node)
					return false;
				if(!isVoidType(v.type)) {
					fail("return with a value in a function returning void");
					return false;
				}
			}
			fn.retVoid();
			return true;
		}
		Node* value;
		if(s->expr) {
			Value v = emitExpr(fn, s->expr);
			if(!v.node)
				return false;
			value = convert(fn, v.node, v.type, curRet);
		} else {
			value = fn.constInt(irType(curRet), 0);
		}
		fn.ret(value);
		return true;
	}

	B32 Emitter::emitExprStmt(Function& fn, const Stmt* s) {
		Value v = emitExpr(fn, s->expr);
		return v.node != nullptr;
	}

	B32 Emitter::emitLabel(Function& fn, const Stmt* s) {
		auto it = labelBlocks.find(*s->label);
		if(it == labelBlocks.end()) {
			fail("internal: missing block for label '" + *s->label + "'");
			return false;
		}
		Function::Block* lbl = it->second;
		if(!fn.blockFinished())
			fn.jmp(lbl);
		fn.setInsertBlock(lbl);
		return emitStmt(fn, s->thenBody);
	}

	B32 Emitter::emitGoto(Function& fn, const Stmt* s) {
		auto it = labelBlocks.find(*s->label);
		if(it == labelBlocks.end()) {
			fail("use of undeclared label '" + *s->label + "'");
			return false;
		}
		fn.jmp(it->second);
		return true;
	}
} // namespace rat::cc
