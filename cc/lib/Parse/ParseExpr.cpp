#include "Parse/Parser.h"

#include "Parse/ParserDetail.h"

namespace rat::cc {
	namespace detail {
		BinInfo binInfo(TokKind kind) {
			// clang-format off
			static const BinInfo kBin[] = {
					{9, ExprOp::Add},    // Plus
					{9, ExprOp::Sub},    // Minus
					{10, ExprOp::Mul},   // Star
					{10, ExprOp::Div},   // Slash
					{10, ExprOp::Rem},   // Percent
					{0, ExprOp::Add},    // PlusPlus
					{0, ExprOp::Add},    // MinusMinus
					{5, ExprOp::BitAnd}, // Amp
					{3, ExprOp::BitOr},  // Pipe
					{4, ExprOp::BitXor}, // Caret
					{0, ExprOp::Add},    // Tilde
					{0, ExprOp::Add},    // Bang
					{2, ExprOp::LogAnd}, // AmpAmp
					{1, ExprOp::LogOr},  // PipePipe
					{7, ExprOp::Lt},     // Lt
					{7, ExprOp::Gt},     // Gt
					{7, ExprOp::Le},     // Le
					{7, ExprOp::Ge},     // Ge
					{6, ExprOp::Eq},     // EqEq
					{6, ExprOp::Ne},     // BangEq
					{8, ExprOp::Shl},    // Shl
					{8, ExprOp::Shr},    // Shr
			};
			// clang-format on
			static_assert(sizeof(kBin) / sizeof(kBin[0]) == (U32)TokKind::Shr - (U32)TokKind::Plus + 1,
										"kBin must cover Plus..Shr");
			if(kind < TokKind::Plus || kind > TokKind::Shr)
				return {0, ExprOp::Add};
			return kBin[(U32)kind - (U32)TokKind::Plus];
		}

		B32 assignOp(TokKind kind, ExprOp& op) {
			if(kind < TokKind::Assign || kind > TokKind::ShrEq)
				return false;
			// clang-format off
			static const ExprOp kOps[] = {
					ExprOp::Assign,    ExprOp::AddAssign, ExprOp::SubAssign, ExprOp::MulAssign,
					ExprOp::DivAssign, ExprOp::RemAssign, ExprOp::AndAssign, ExprOp::OrAssign,
					ExprOp::XorAssign, ExprOp::ShlAssign, ExprOp::ShrAssign,
			};
			// clang-format on
			static_assert(sizeof(kOps) / sizeof(kOps[0]) ==
												(U32)TokKind::ShrEq - (U32)TokKind::Assign + 1,
										"kOps must cover Assign..ShrEq");
			op = kOps[(U32)kind - (U32)TokKind::Assign];
			return true;
		}

		B32 unaryOp(TokKind kind, ExprOp& op) {
			struct Entry {
				TokKind kind;
				ExprOp op;
			};
			static const Entry kUnary[] = {
					{TokKind::Plus, ExprOp::Pos},
					{TokKind::Minus, ExprOp::Neg},
					{TokKind::Bang, ExprOp::Not},
					{TokKind::Tilde, ExprOp::BitNot},
					{TokKind::Amp, ExprOp::Addr},
					{TokKind::Star, ExprOp::Deref},
					{TokKind::KwReal, ExprOp::Real},
					{TokKind::KwImag, ExprOp::Imag},
			};
			for(const Entry& e : kUnary)
				if(e.kind == kind) {
					op = e.op;
					return true;
				}
			return false;
		}

		String decodeUtf8ToUtf32LE(const String& bytes) {
			String w;
			U32 i = 0, n = (U32)bytes.size();
			while(i < n) {
				U8 b0 = (U8)bytes[i];
				U32 cp = 0, extra = 0;
				if(b0 < 0x80) {
					cp = b0;
				} else if((b0 & 0xE0) == 0xC0) {
					cp = b0 & 0x1F;
					extra = 1;
				} else if((b0 & 0xF0) == 0xE0) {
					cp = b0 & 0x0F;
					extra = 2;
				} else if((b0 & 0xF8) == 0xF0) {
					cp = b0 & 0x07;
					extra = 3;
				} else {
					cp = b0;
				}
				++i;
				for(U32 k = 0; k < extra && i < n; ++k, ++i)
					cp = (cp << 6) | ((U8)bytes[i] & 0x3F);
				for(U32 k = 0; k < 4; ++k)
					w.push_back((char)(U8)(cp >> (8 * k)));
			}
			return w;
		}

		String decodeUtf8ToUtf16LE(const String& bytes) {
			String u32 = decodeUtf8ToUtf32LE(bytes);
			String w;
			for(U32 i = 0; i + 3 < (U32)u32.size(); i += 4) {
				U32 cp = (U8)u32[i] | ((U32)(U8)u32[i + 1] << 8) | ((U32)(U8)u32[i + 2] << 16) |
								 ((U32)(U8)u32[i + 3] << 24);
				if(cp >= 0x10000) {
					U32 v = cp - 0x10000;
					U32 hi = 0xD800 + (v >> 10), lo = 0xDC00 + (v & 0x3FF);
					w.push_back((char)(U8)hi);
					w.push_back((char)(U8)(hi >> 8));
					w.push_back((char)(U8)lo);
					w.push_back((char)(U8)(lo >> 8));
				} else {
					w.push_back((char)(U8)cp);
					w.push_back((char)(U8)(cp >> 8));
				}
			}
			return w;
		}
	} // namespace detail

	Expr* Parser::parseUnary() {
		if(peek().kind == TokKind::KwSizeof) {
			Token kw = advance(); // sizeof
			Expr* e = makeExpr(ExprKind::Sizeof, kw.offset);
			if(peek().kind == TokKind::LParen && startsType(peek2())) {
				advance(); // (
				CType ty;
				if(!parseTypeName(ty)) // sizeof(typename)
					return nullptr;
				if(!expect(TokKind::RParen, "')'"))
					return nullptr;
				e->sizeOf.type = ty;
				e->sizeOf.operand = nullptr;
			} else {
				Expr* operand = parseUnary();
				if(!operand)
					return nullptr;
				e->sizeOf.operand = operand;
			}
			return e;
		}
		if(peek().kind == TokKind::LParen && startsType(peek2())) {
			Token lp = advance(); // (
			CType ty;
			if(!parseTypeSpec(ty))
				return nullptr;
			parsePointers(ty);
			if(looksLikeFuncPtr()) { // cast to a function-pointer/grouped type
				Token ignored;
				B32 hn = false;
				CType ft;
				if(!parseDeclaratorType(ty, ignored, hn, ft))
					return nullptr;
				ty = ft;
				if(!expect(TokKind::RParen, "')'"))
					return nullptr;
				Expr* operand = parseUnary();
				if(!operand)
					return nullptr;
				Expr* e = makeExpr(ExprKind::Cast, lp.offset);
				e->cast.type = ty;
				e->cast.operand = operand;
				return e;
			}
			B32 isArr = false;
			Expr* arrLen = nullptr;
			if(accept(TokKind::LBracket)) { // array type-name: (T[]) or (T[N])
				isArr = true;
				if(peek().kind != TokKind::RBracket) {
					arrLen = parseConditional();
					if(!arrLen)
						return nullptr;
				}
				if(!expect(TokKind::RBracket, "']'"))
					return nullptr;
			}
			if(!expect(TokKind::RParen, "')'"))
				return nullptr;
			if(peek().kind == TokKind::LBrace) { // compound literal
				Expr* init = parseInitializer();
				if(!init)
					return nullptr;
				Expr* e = makeExpr(ExprKind::CompoundLit, lp.offset);
				e->compound.type = ty;
				e->compound.init = init;
				e->compound.arrayLen = arrLen;
				e->compound.isArray = isArr;
				return parsePostfixTail(e); // allow [..], .x, etc. after the literal
			}
			if(isArr) {
				fail(lp, "array types may not be used in a cast");
				return nullptr;
			}
			Expr* operand = parseUnary();
			if(!operand)
				return nullptr;
			Expr* e = makeExpr(ExprKind::Cast, lp.offset);
			e->cast.type = ty;
			e->cast.operand = operand;
			return e;
		}
		ExprOp op;
		if(detail::unaryOp(peek().kind, op)) {
			Token t = advance();
			Expr* operand = parseUnary();
			if(!operand)
				return nullptr;
			return makeUnary(t.offset, op, operand);
		}
		TokKind k = peek().kind;
		if(k == TokKind::PlusPlus || k == TokKind::MinusMinus) {
			Token t = advance();
			Expr* operand = parseUnary();
			if(!operand)
				return nullptr;
			ExprOp pre = k == TokKind::PlusPlus ? ExprOp::PreInc : ExprOp::PreDec;
			return makeUnary(t.offset, pre, operand);
		}
		return parsePostfix();
	}

	Expr* Parser::parseBinary(I32 minPrec) {
		Expr* lhs = parseUnary();
		if(!lhs)
			return nullptr;
		for(;;) {
			detail::BinInfo info = detail::binInfo(peek().kind);
			if(info.prec == 0 || info.prec < minPrec)
				break;
			Token opTok = advance();
			Expr* rhs = parseBinary(info.prec + 1); // left-associative
			if(!rhs)
				return nullptr;
			lhs = makeBinary(opTok.offset, info.op, lhs, rhs);
		}
		return lhs;
	}

	Expr* Parser::parseConditional() {
		Expr* cond = parseBinary(1);
		if(!cond)
			return nullptr;
		if(peek().kind != TokKind::Question)
			return cond;
		Token q = advance();
		Expr* whenTrue = parseExpression();
		if(!whenTrue)
			return nullptr;
		if(!expect(TokKind::Colon, "':'"))
			return nullptr;
		Expr* whenFalse = parseConditional();
		if(!whenFalse)
			return nullptr;
		Expr* e = makeExpr(ExprKind::Ternary, q.offset);
		e->ternary = {cond, whenTrue, whenFalse};
		return e;
	}

	Expr* Parser::parseAssignment() {
		Expr* lhs = parseConditional();
		if(!lhs)
			return nullptr;
		ExprOp op;
		if(detail::assignOp(peek().kind, op)) {
			Token opTok = advance();
			Expr* rhs = parseAssignment(); // right-associative
			if(!rhs)
				return nullptr;
			return makeBinary(opTok.offset, op, lhs, rhs);
		}
		return lhs;
	}

	Expr* Parser::parseExpression() {
		Expr* e = parseAssignment();
		if(!e)
			return nullptr;
		while(peek().kind == TokKind::Comma) {
			Token c = advance();
			Expr* rhs = parseAssignment();
			if(!rhs)
				return nullptr;
			Expr* comma = makeExpr(ExprKind::Comma, c.offset);
			comma->comma = {e, rhs};
			e = comma;
		}
		return e;
	}
} // namespace rat::cc
