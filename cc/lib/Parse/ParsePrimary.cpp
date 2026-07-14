#include "Parse/Parser.h"

#include "Parse/ParserDetail.h"

namespace rat::cc {
	Expr* Parser::parseBuiltinOffsetof(const Token& kw) {
		if(!expect(TokKind::LParen, "'('"))
			return nullptr;
		CType ty;
		if(!parseTypeSpec(ty)) {
			fail(peek(), "expected a type in __builtin_offsetof");
			return nullptr;
		}
		if(!expect(TokKind::Comma, "','"))
			return nullptr;
		// member-designator: identifier ('.' identifier | '[' const ']')*
		if(peek().kind != TokKind::Identifier) {
			fail(peek(), "expected a member name in __builtin_offsetof");
			return nullptr;
		}
		I64 off = 0;
		CType cur = ty;
		B32 first = true;
		for(;;) {
			if(first || accept(TokKind::Dot)) {
				first = false;
				if(peek().kind != TokKind::Identifier) {
					fail(peek(), "expected a member name in __builtin_offsetof");
					return nullptr;
				}
				Token m = advance();
				if(!isStruct(cur)) {
					fail(m, "__builtin_offsetof of a member of a non-struct type");
					return nullptr;
				}
				const Field* f = cur.strukt->find(lex.text(m));
				if(!f) {
					fail(m, "no such member in __builtin_offsetof");
					return nullptr;
				}
				off += f->offset;
				cur = f->type; // element type for array members
			} else if(accept(TokKind::LBracket)) {
				Expr* ix = parseConditional();
				I64 v = 0;
				if(!ix || !evalIntConst(ix, v)) {
					fail(peek(), "array index in __builtin_offsetof must be constant");
					return nullptr;
				}
				if(!expect(TokKind::RBracket, "']'"))
					return nullptr;
				off += v * (I64)typeSizeBytes(cur);
			} else {
				break;
			}
		}
		if(!expect(TokKind::RParen, "')'"))
			return nullptr;
		return makeInt(kw, off, true, true);
	}

	B32 Parser::parseTypeName(CType& out) {
		CType ty;
		if(!parseTypeSpec(ty))
			return false;
		parsePointers(ty);
		if(looksLikeFuncPtr()) { // abstract function-pointer/grouped type
			Token ignored;
			B32 hn = false;
			CType ft;
			if(!parseDeclaratorType(ty, ignored, hn, ft))
				return false;
			ty = ft;
		} else if(check(TokKind::LBracket)) { // array type-name: T[N] or T[]
			List<Dim> dims;
			while(accept(TokKind::LBracket)) {
				Dim d{0, nullptr};
				if(peek().kind != TokKind::RBracket) {
					Expr* len = parseConditional();
					if(!len)
						return false;
					I64 v = 0;
					if(tryEvalIntConst(len, v) && v > 0)
						d.count = (U32)v;
					else
						d.expr = len; // VLA
				}
				if(!expect(TokKind::RBracket, "']'"))
					return false;
				dims.push_back(d);
			}
			ty = wrapArrayDims(ty, dims);
		}
		out = ty;
		return true;
	}

	Expr* Parser::parseGeneric() {
		Token kw = advance(); // _Generic
		if(!expect(TokKind::LParen, "'('"))
			return nullptr;
		Expr* control = parseAssignment();
		if(!control)
			return nullptr;
		Expr* e = makeExpr(ExprKind::Generic, kw.offset);
		e->generic.control = control;
		while(accept(TokKind::Comma)) {
			GenericAssoc assoc;
			if(peek().kind == TokKind::KwDefault) {
				advance();
				assoc.isDefault = true;
			} else {
				if(!parseTypeName(assoc.type))
					return nullptr;
			}
			if(!expect(TokKind::Colon, "':'"))
				return nullptr;
			Expr* result = parseAssignment();
			if(!result)
				return nullptr;
			assoc.result = result;
			e->assocs.push_back(assoc);
		}
		if(!expect(TokKind::RParen, "')'"))
			return nullptr;
		return e;
	}

	Expr* Parser::parsePrimary() {
		const Token& tok = peek();
		if(tok.kind == TokKind::KwGeneric)
			return parseGeneric();
		if(tok.kind == TokKind::StringLiteral) {
			Token first = advance();
			String head = lex.text(first);
			B32 wide = !head.empty() && head[0] == 'L';
			String bytes;
			if(!parseStringLiteral(first, bytes))
				return nullptr;
			while(peek().kind == TokKind::StringLiteral) {
				String h2 = lex.text(peek());
				if(!h2.empty() && h2[0] == 'L')
					wide = true;
				if(!parseStringLiteral(advance(), bytes))
					return nullptr;
			}
			Expr* e = makeExpr(ExprKind::StrLit, first.offset);
			if(wide) {
				String w = detail::decodeUtf8ToUtf32LE(bytes);
				e->str.bytes = arena.make<String>(std::move(w));
				e->str.isWide = true;
				e->str.charSize = 4;
			} else {
				e->str.bytes = arena.make<String>(std::move(bytes));
				e->str.isWide = false;
				e->str.charSize = 1;
			}
			return e;
		}
		if(tok.kind == TokKind::CharConstant) {
			Token lit = advance();
			I64 value;
			if(!parseCharLiteral(lit, value))
				return nullptr;
			return makeInt(lit, value, false, false);
		}
		if(tok.kind == TokKind::IntConstant) {
			Token lit = advance();
			I64 value;
			B32 isUnsigned, isLong;
			if(!parseIntLiteral(lit, value, isUnsigned, isLong))
				return nullptr;
			return makeInt(lit, value, isUnsigned, isLong);
		}
		if(tok.kind == TokKind::FloatConstant) {
			Token lit = advance();
			String text = lex.text(lit);
			B32 isFloat = false, isLongDouble = false, isImaginary = false;
			while(!text.empty()) {
				char c = text.back();
				if(c == 'f' || c == 'F')
					isFloat = true;
				else if(c == 'l' || c == 'L')
					isLongDouble = true;
				else if(c == 'i' || c == 'I' || c == 'j' || c == 'J')
					isImaginary = true;
				else
					break;
				text.pop_back();
			}
			long double value = std::stold(text);
			Expr* e = makeExpr(ExprKind::FloatLit, lit.offset);
			e->floatLit = {value, isFloat, isLongDouble, isImaginary};
			return e;
		}
		if(tok.kind == TokKind::Identifier) {
			Token id = advance();
			// __func__
			if(lex.text(id) == "__func__") {
				Expr* e = makeExpr(ExprKind::StrLit, id.offset);
				e->str.bytes = arena.make<String>(curFuncName);
				return e;
			}
			// __builtin_offsetof(type, member)
			if(lex.text(id) == "__builtin_offsetof")
				return parseBuiltinOffsetof(id);
			auto ec = enumConstants.find(lex.text(id));
			if(ec != enumConstants.end())
				return makeInt(id, ec->second, false, false);
			return makeIdent(id);
		}
		if(tok.kind == TokKind::LParen) {
			advance();
			// GNU statement expression
			if(peek().kind == TokKind::LBrace) {
				Stmt* body = parseCompound();
				if(!body)
					return nullptr;
				if(!expect(TokKind::RParen, "')'"))
					return nullptr;
				Expr* e = makeExpr(ExprKind::StmtExpr, tok.offset);
				e->stmtExpr.body = body;
				return e;
			}
			Expr* inner = parseExpression();
			if(!inner)
				return nullptr;
			if(!expect(TokKind::RParen, "')'"))
				return nullptr;
			return inner;
		}
		fail(tok, String("expected expression, found '") + tokKindName(tok.kind) + "'");
		return nullptr;
	}

	Expr* Parser::parsePostfix() {
		Expr* e = parsePrimary();
		if(!e)
			return nullptr;
		return parsePostfixTail(e);
	}

	Expr* Parser::parsePostfixTail(Expr* e) {
		for(;;) {
			TokKind k = peek().kind;
			if(k == TokKind::LParen) {
				// __builtin_va_arg(ap, type)
				if(e->kind == ExprKind::Ident && *e->ident.name == "__builtin_va_arg") {
					Token lp = advance(); // '('
					Expr* ap = parseAssignment();
					if(!ap)
						return nullptr;
					if(!expect(TokKind::Comma, "','"))
						return nullptr;
					CType ty;
					if(!parseTypeSpec(ty)) {
						fail(peek(), "expected a type in __builtin_va_arg");
						return nullptr;
					}
					parsePointers(ty);
					if(!expect(TokKind::RParen, "')'"))
						return nullptr;
					Expr* va = makeExpr(ExprKind::VaArg, lp.offset);
					va->vaArg.ap = ap;
					va->vaArg.type = ty;
					e = va;
					continue;
				}
				Token lp = advance();
				Expr* callE = makeExpr(ExprKind::Call, lp.offset);
				if(e->kind == ExprKind::Ident) {
					// by-name call
					callE->call.callee = e->ident.name;
					callE->call.target = nullptr;
				} else {
					// indirect call
					callE->call.callee = nullptr;
					callE->call.target = e;
				}
				if(peek().kind != TokKind::RParen) {
					for(;;) {
						Expr* arg = parseAssignment();
						if(!arg)
							return nullptr;
						callE->args.push_back(arg);
						if(!accept(TokKind::Comma))
							break;
					}
				}
				if(!expect(TokKind::RParen, "')'"))
					return nullptr;
				e = callE;
			} else if(k == TokKind::LBracket) {
				Token lb = advance();
				Expr* idx = parseExpression();
				if(!idx)
					return nullptr;
				if(!expect(TokKind::RBracket, "']'"))
					return nullptr;
				Expr* sum = makeBinary(lb.offset, ExprOp::Add, e, idx);
				e = makeUnary(lb.offset, ExprOp::Deref, sum);
			} else if(k == TokKind::Dot || k == TokKind::Arrow) {
				Token t = advance();
				if(peek().kind != TokKind::Identifier) {
					fail(peek(),
							 "expected member name after '" + String(k == TokKind::Arrow ? "->" : ".") + "'");
					return nullptr;
				}
				Token nameTok = advance();
				Expr* m = makeExpr(ExprKind::Member, t.offset);
				m->member.base = e;
				m->member.name = arena.make<String>(lex.text(nameTok));
				m->member.arrow = k == TokKind::Arrow;
				e = m;
			} else if(k == TokKind::PlusPlus || k == TokKind::MinusMinus) {
				Token t = advance();
				ExprOp op = k == TokKind::PlusPlus ? ExprOp::PostInc : ExprOp::PostDec;
				e = makeUnary(t.offset, op, e);
			} else {
				break;
			}
		}
		return e;
	}
} // namespace rat::cc
