#include "Parser.h"

namespace rat::cc {
	Expr* Parser::parseInitializer() {
		if(peek().kind == TokKind::LBrace) {
			Token lb = advance();
			Expr* e = makeExpr(ExprKind::InitList, lb.offset);
			if(peek().kind != TokKind::RBrace) {
				for(;;) {
					Designator des;
					I64 rangeEnd = -1; // GNU range designator [a ... b]
					if(peek().kind == TokKind::Dot || peek().kind == TokKind::LBracket) {
						Designator* cur = &des;
						for(B32 first = true;; first = false) {
							if(accept(TokKind::Dot)) {
								Token id = peek();
								if(id.kind != TokKind::Identifier) {
									fail(id, "expected a field name after '.'");
									return nullptr;
								}
								advance();
								cur->isIndex = false;
								cur->field = arena.make<String>(lex.text(id));
							} else if(accept(TokKind::LBracket)) {
								Expr* ix = parseAssignment();
								I64 v = 0;
								if(!ix || !evalIntConst(ix, v) || v < 0) {
									fail(peek(),
											 "array designator must be a non-negative "
											 "integer constant");
									return nullptr;
								}
								cur->isIndex = true;
								cur->index = v;
								if(first && accept(TokKind::Ellipsis)) {
									Expr* hi = parseAssignment();
									I64 hv = 0;
									if(!hi || !evalIntConst(hi, hv) || hv < v) {
										fail(peek(), "invalid range designator");
										return nullptr;
									}
									rangeEnd = hv;
								}
								if(!expect(TokKind::RBracket, "']'"))
									return nullptr;
							} else {
								break;
							}
							if(peek().kind == TokKind::Dot || peek().kind == TokKind::LBracket) {
								Designator* nxt = arena.make<Designator>();
								nxt->isSet = true;
								cur->next = nxt;
								cur = nxt;
								continue;
							}
							break;
						}
						if(!expect(TokKind::Assign, "'=' after designator"))
							return nullptr;
						des.isSet = true;
					}
					Expr* el = parseInitializer();
					if(!el)
						return nullptr;
					if(rangeEnd >= des.index) {
						for(I64 k = des.index; k <= rangeEnd; ++k) {
							Designator d2;
							d2.isSet = true;
							d2.isIndex = true;
							d2.index = k;
							e->args.push_back(el);
							e->designators.push_back(d2);
						}
					} else {
						e->args.push_back(el);
						e->designators.push_back(des);
					}
					if(!accept(TokKind::Comma))
						break;
					if(peek().kind == TokKind::RBrace)
						break;
				}
			}
			if(!expect(TokKind::RBrace, "'}'"))
				return nullptr;
			return e;
		}
		return parseAssignment();
	}

	B32 Parser::evalIntConst(const Expr* e, I64& out) {
		switch(e->kind) {
		case ExprKind::IntLit:
			out = e->intLit.value;
			return true;
		case ExprKind::Unary: {
			I64 v;
			if(!evalIntConst(e->unary.operand, v))
				return false;
			switch(e->unary.op) {
				// clang-format off
			case ExprOp::Pos:    out = v;  return true;
			case ExprOp::Neg:    out = -v; return true;
			case ExprOp::Not:    out = !v; return true;
			case ExprOp::BitNot: out = ~v; return true;
			// clang-format on
			default:
				fail(peek(), "non-constant operator in constant expression");
				return false;
			}
		}
		case ExprKind::Binary: {
			I64 l, r;
			if(!evalIntConst(e->binary.lhs, l) || !evalIntConst(e->binary.rhs, r))
				return false;
			switch(e->binary.op) {
				// clang-format off
			case ExprOp::Add:    out = l + r;  return true;
			case ExprOp::Sub:    out = l - r;  return true;
			case ExprOp::Mul:    out = l * r;  return true;
			case ExprOp::Shl:    out = l << r; return true;
			case ExprOp::Shr:    out = l >> r; return true;
			case ExprOp::Lt:     out = l < r;  return true;
			case ExprOp::Gt:     out = l > r;  return true;
			case ExprOp::Le:     out = l <= r; return true;
			case ExprOp::Ge:     out = l >= r; return true;
			case ExprOp::Eq:     out = l == r; return true;
			case ExprOp::Ne:     out = l != r; return true;
			case ExprOp::BitAnd: out = l & r;  return true;
			case ExprOp::BitOr:  out = l | r;  return true;
			case ExprOp::BitXor: out = l ^ r;  return true;
			case ExprOp::LogAnd: out = l && r; return true;
			case ExprOp::LogOr:  out = l || r; return true;
			// clang-format on
			case ExprOp::Div:
			case ExprOp::Rem:
				if(r == 0) {
					fail(peek(), "division by zero in constant expression");
					return false;
				}
				out = (e->binary.op == ExprOp::Div) ? (l / r) : (l % r);
				return true;
			default:
				fail(peek(), "non-constant operator in constant expression");
				return false;
			}
		}
		case ExprKind::Ternary: {
			I64 c;
			if(!evalIntConst(e->ternary.cond, c))
				return false;
			return evalIntConst(c ? e->ternary.whenTrue : e->ternary.whenFalse, out);
		}
		case ExprKind::Cast:
			return evalIntConst(e->cast.operand, out);
		case ExprKind::Sizeof:
			if(e->sizeOf.operand) {
				fail(peek(), "sizeof expression not allowed in constant expression");
				return false;
			}
			if(isVlaType(e->sizeOf.type)) {
				fail(peek(), "sizeof a variable-length array is not constant");
				return false;
			}
			if(isStruct(e->sizeOf.type) && !e->sizeOf.type.strukt->complete) {
				fail(peek(), "sizeof applied to an incomplete type");
				return false;
			}
			out = (I64)typeSizeBytes(e->sizeOf.type);
			return true;
		default:
			fail(peek(), "expected a constant expression");
			return false;
		}
	}

	B32 Parser::tryEvalIntConst(const Expr* e, I64& out) {
		B32 savedFailed = failed;
		String savedMsg = errMsg;
		B32 ok = evalIntConst(e, out);
		if(!ok) {
			failed = savedFailed;
			errMsg = std::move(savedMsg);
		}
		return ok;
	}
} // namespace rat::cc
