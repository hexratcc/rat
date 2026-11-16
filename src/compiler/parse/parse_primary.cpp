#include "parse/parser.h"

#include "parse/parser_detail.h"

#include <cstdlib>

namespace rat::cc {
	// ( type-spec , name [ . name | '[' const-expr ']' ]... )
	// folded to an integer
	Expr* Parser::parseBuiltinOffsetof(const Token& kw) {
		if(!expect(TokKind::LParen, "'('"))
			return nullptr;
		CType ty;
		if(!parseTypeSpec(ty))
			return fail(peek(), "expected a type in __builtin_offsetof");
		if(!expect(TokKind::Comma, "','"))
			return nullptr;
		I64 off = 0;
		CType cur = ty;
		for(B32 first = true;; first = false) {
			if(first || accept(TokKind::Dot)) {
				if(!check(TokKind::Identifier))
					return fail(peek(), "expected a member name in __builtin_offsetof");
				Token m = advance();
				if(!isStruct(cur))
					return fail(m, "__builtin_offsetof of a member of a non-struct type");
				const Field* f = cur.strukt->find(lex.text(m));
				if(!f)
					return fail(m, "no such member in __builtin_offsetof");
				off += f->offset;
				cur = f->type; // element type for array members
			} else if(accept(TokKind::LBracket)) {
				Expr* ix = parseConditional();
				I64 v = 0;
				if(!ix || !evalIntConst(ix, v))
					return fail(peek(), "array index in __builtin_offsetof must be constant");
				if(!expect(TokKind::RBracket, "']'"))
					return nullptr;
				off += v * (I64)typeSizeBytes(cur);
			} else {
				break;
			}
		}
		if(!expect(TokKind::RParen, "')'"))
			return nullptr;
		U8 sizeMods = (U8)(CType::Unsigned | CType::Long | (lay.longBits < 64 ? CType::LongLong : 0));
		return makeInt(kw, off, 64, sizeMods);
	}

	// type-spec abstract-declarator
	B32 Parser::parseTypeName(CType& out) {
		CType ty;
		DeclResult r;
		if(!parseTypeSpec(ty) || !parseAbstractDeclarator(ty, r))
			return false;
		out = r.type;
		return true;
	}

	// _Generic ( assignment [, assoc]... )
	// assoc: type-name : assignment | default : assignment
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
			if(accept(TokKind::KwDefault))
				assoc.isDefault = true;
			else if(!parseTypeName(assoc.type))
				return nullptr;
			if(!expect(TokKind::Colon, "':'"))
				return nullptr;
			assoc.result = parseAssignment();
			if(!assoc.result)
				return nullptr;
			e->assocs.push_back(assoc);
		}
		if(!expect(TokKind::RParen, "')'"))
			return nullptr;
		return e;
	}

	// string-literal...
	Expr* Parser::parsePrimaryString() {
		List<Token> parts;
		while(check(TokKind::StringLiteral))
			parts.push_back(advance());
		U32 unitBytes = 1;
		for(const Token& t : parts) {
			const String& h = lex.text(t);
			if(h.empty())
				continue;
			if(h[0] == 'L')
				unitBytes = lay.wcharBytes;
			else if(h[0] == 'U')
				unitBytes = 4;
			else if(h[0] == 'u' && !(h.size() > 1 && h[1] == '8'))
				unitBytes = 2;
		}
		String bytes;
		for(const Token& t : parts) {
			if(unitBytes == 1 && !parseStringLiteral(t, bytes))
				return nullptr;
			if(unitBytes != 1 && !parseWideStringLiteral(t, unitBytes, bytes))
				return nullptr;
		}
		Expr* e = makeExpr(ExprKind::StrLit, parts[0].offset);
		e->str = {arena.make<String>(std::move(bytes)), unitBytes != 1, unitBytes};
		return e;
	}

	// float-const
	Expr* Parser::parsePrimaryFloat() {
		Token lit = advance();
		String text = lex.text(lit);
		B32 isFloat = false;
		B32 isLongDouble = false;
		B32 isImaginary = false;
		while(!text.empty()) {
			C8 c = text.back();
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
		C8* end = nullptr;
		F80 value = std::strtold(text.c_str(), &end);
		if(end == text.c_str() || *end != '\0')
			return fail(lit, "invalid floating constant '" + text + "'");
		Expr* e = makeExpr(ExprKind::FloatLit, lit.offset);
		U32 bits = 64;
		if(isFloat)
			bits = 32;
		else if(isLongDouble)
			bits = 128;
		e->floatLit = {value, bits, isImaginary};
		return e;
	}

	// name
	// __func__, __builtin_offsetof and enum constants fold here
	Expr* Parser::parsePrimaryName() {
		Token id = advance();
		const String& name = lex.text(id);
		if(name == "__func__") {
			Expr* e = makeExpr(ExprKind::StrLit, id.offset);
			e->str = {arena.make<String>(curFuncName), false, 1};
			return e;
		}
		if(name == "__builtin_offsetof")
			return parseBuiltinOffsetof(id);
		if(const I64* ec = enumConstants.get(name))
			return makeInt(id, *ec, 32);
		Expr* e = makeExpr(ExprKind::Ident, id.offset);
		e->ident.name = arena.make<String>(name);
		return e;
	}

	// ( expr ) | ( compound )
	Expr* Parser::parsePrimaryParen() {
		if(peek2().kind != TokKind::LBrace)
			return parseParenCond();
		Token open = advance(); // ( of a GNU statement expression
		Stmt* body = parseCompound();
		if(!body || !expect(TokKind::RParen, "')'"))
			return nullptr;
		Expr* e = makeExpr(ExprKind::StmtExpr, open.offset);
		e->stmtExpr.body = body;
		return e;
	}

	// generic | string-literal... | char-const | int-const | float-const | name
	// | ( expr ) | ( compound )
	Expr* Parser::parsePrimary() {
		const Token& tok = peek();
		switch(tok.kind) {
		case TokKind::KwGeneric:
			return parseGeneric();
		case TokKind::StringLiteral:
			return parsePrimaryString();
		case TokKind::CharConstant: {
			Token lit = advance();
			I64 value;
			if(!parseCharLiteral(lit, value))
				return nullptr;
			return makeInt(lit, value, 32);
		}
		case TokKind::IntConstant: {
			Token lit = advance();
			I64 value;
			U32 bits;
			U8 mods;
			if(!parseIntLiteral(lit, value, bits, mods))
				return nullptr;
			return makeInt(lit, value, bits, mods);
		}
		case TokKind::FloatConstant:
			return parsePrimaryFloat();
		case TokKind::Identifier:
			return parsePrimaryName();
		case TokKind::LParen:
			return parsePrimaryParen();
		default:
			return fail(tok, String("expected expression, found '") + tokKindName(tok.kind) + "'");
		}
	}

	// __builtin_va_arg ( assignment , type-name )
	Expr* Parser::parseVaArg() {
		Token lp = advance(); // (
		Expr* ap = parseAssignment();
		if(!ap || !expect(TokKind::Comma, "','"))
			return nullptr;
		CType ty;
		if(!parseTypeName(ty))
			return fail(peek(), "expected a type in __builtin_va_arg");
		if(!expect(TokKind::RParen, "')'"))
			return nullptr;
		Expr* va = makeExpr(ExprKind::VaArg, lp.offset);
		va->vaArg = {ap, ty};
		return va;
	}

	// ( [ assignment [, assignment]... ] )
	Expr* Parser::parseCall(Expr* e) {
		Token lp = advance(); // (
		Expr* callE = makeExpr(ExprKind::Call, lp.offset);
		if(e->kind == ExprKind::Ident) // by-name call
			callE->call = {e->ident.name, nullptr};
		else // indirect call
			callE->call = {nullptr, e};
		if(!check(TokKind::RParen)) {
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
		return callE;
	}

	// [ call | '[' expr ']' | . name | -> name | ++ | -- ]...
	// __builtin_va_arg ( assignment , type-name ) is a call in form only
	Expr* Parser::parsePostfixTail(Expr* e) {
		for(;;) {
			TokKind k = peek().kind;
			if(k == TokKind::LParen) {
				if(e->kind == ExprKind::Ident && *e->ident.name == "__builtin_va_arg")
					e = parseVaArg();
				else
					e = parseCall(e);
				if(!e)
					return nullptr;
			} else if(k == TokKind::LBracket) {
				Token lb = advance();
				Expr* idx = parseExpression();
				if(!idx || !expect(TokKind::RBracket, "']'"))
					return nullptr;
				Expr* sum = makeBinary(lb.offset, ExprOp::Add, e, idx);
				e = makeUnary(lb.offset, ExprOp::Deref, sum);
			} else if(k == TokKind::Dot || k == TokKind::Arrow) {
				Token t = advance();
				const C8* opSpelling = k == TokKind::Arrow ? "->" : ".";
				if(!check(TokKind::Identifier))
					return fail(peek(), "expected member name after '" + String(opSpelling) + "'");
				Token nameTok = advance();
				Expr* m = makeExpr(ExprKind::Member, t.offset);
				m->member = {e, arena.make<String>(lex.text(nameTok)), k == TokKind::Arrow};
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
