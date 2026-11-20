#include "parse/parser.h"

#include "parse/parser_detail.h"

#include <cstdlib>

namespace rat::cc {
	// ( type-spec , name [ . name | '[' const-expr ']' ]... )
	// folded to an integer
	Expr* Parser::parseBuiltinOffsetof(const Token& kw) {
		if(!expect(TokKind::LParen))
			return nullptr;
		CType ty;
		if(!parseTypeSpec(ty))
			return fail(peek(), "expected a type in __builtin_offsetof");
		if(!expect(TokKind::Comma))
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
				cur = f->type;
			} else if(accept(TokKind::LBracket)) {
				Expr* ix = parseConditional();
				I64 v = 0;
				if(!ix || !evalIntConst(ix, v))
					return fail(peek(), "array index in __builtin_offsetof must be constant");
				if(!expect(TokKind::RBracket))
					return nullptr;
				if(isArrayType(cur))
					cur = arrayElem(cur);
				off += v * (I64)typeSizeBytes(cur);
			} else {
				break;
			}
		}
		if(!expect(TokKind::RParen))
			return nullptr;
		U8 sizeMods = (U8)(CType::Unsigned | CType::Long | (lay.longBits < 64 ? CType::LongLong : 0));
		return makeInt(kw, off, 64, sizeMods);
	}

	// type-spec abstract-declarator
	B32 Parser::parseTypeName(CType& out) {
		CType ty;
		DeclResult r;
		if(!parseTypeSpec(ty) || !parseDeclarator(ty, r))
			return false;
		if(r.name)
			return fail(peek(), "unexpected name in a type name");
		out = r.type;
		return true;
	}

	// type-name : assignment | default : assignment
	// the first association whose type matches control is selected
	B32 Parser::parseGenericAssoc(CType control, Expr*& selected, Expr*& fallback) {
		B32 isDefault = accept(TokKind::KwDefault);
		CType type;
		if(!isDefault && !parseTypeName(type))
			return false;
		if(!expect(TokKind::Colon))
			return false;
		Expr* result = parseAssignment();
		if(!result)
			return false;
		if(isDefault)
			fallback = result;
		else if(!selected && typesCompatible(control, type))
			selected = result;
		return true;
	}

	// _Generic ( assignment [, assoc]... )
	// resolved here, the selected expression stands for the whole
	Expr* Parser::parseGeneric() {
		Token kw = advance(); // _Generic
		if(!expect(TokKind::LParen))
			return nullptr;
		Expr* control = parseAssignment();
		if(!control)
			return nullptr;
		CType controlType = valueType(control->type);
		clearTopConst(controlType);
		clearTopVolatile(controlType);
		Expr* selected = nullptr;
		Expr* fallback = nullptr;
		while(accept(TokKind::Comma))
			if(!parseGenericAssoc(controlType, selected, fallback))
				return nullptr;
		if(!expect(TokKind::RParen))
			return nullptr;
		if(!selected)
			selected = fallback;
		if(!selected)
			return fail(kw, "no _Generic association matches the controlling type");
		return selected;
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
			if(!parseStringLiteral(t, unitBytes, bytes))
				return nullptr;
		}
		Expr* e = makeExpr(ExprKind::StrLit, parts[0].offset);
		e->str = {arena.make<String>(std::move(bytes)), unitBytes != 1, unitBytes};
		return typed(e);
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
		return typed(e);
	}

	// name
	// __func__, __builtin_offsetof and enum constants fold here
	// only a call may name an undeclared function
	Expr* Parser::parsePrimaryName() {
		Token tok = advance();
		const String& name = lex.text(tok);
		if(name == "__func__") {
			Expr* e = makeExpr(ExprKind::StrLit, tok.offset);
			e->str = {arena.make<String>(curFuncName), false, 1};
			return typed(e);
		}
		if(name == "__builtin_offsetof")
			return parseBuiltinOffsetof(tok);
		const Ident* id = idents.get(name);
		if(id && id->kind == Ident::Kind::EnumConst)
			return makeInt(tok, id->value, 32);
		B32 declared = id && id->kind == Ident::Kind::Object;
		if(!declared && !check(TokKind::LParen))
			return fail(tok, "use of undeclared identifier '" + name + "'");
		Expr* e = makeExpr(ExprKind::Ident, tok.offset);
		e->ident.name = arena.make<String>(name);
		if(declared)
			e->type = id->type;
		return e;
	}

	// ( expr ) | ( compound )
	Expr* Parser::parsePrimaryParen() {
		if(peek2().kind != TokKind::LBrace)
			return parseParenCond();
		Token open = advance(); // ( of a GNU statement expression
		Stmt* body = parseCompound();
		if(!body || !expect(TokKind::RParen))
			return nullptr;
		Expr* e = makeExpr(ExprKind::StmtExpr, open.offset);
		e->stmtExpr.body = body;
		return typed(e);
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
		if(!ap || !expect(TokKind::Comma))
			return nullptr;
		CType ty;
		if(!parseTypeName(ty))
			return fail(peek(), "expected a type in __builtin_va_arg");
		if(!expect(TokKind::RParen))
			return nullptr;
		Expr* va = makeExpr(ExprKind::VaArg, lp.offset);
		va->vaArg = {ap, ty};
		return typed(va);
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
		if(!expect(TokKind::RParen))
			return nullptr;
		return typed(callE);
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
			} else if(k == TokKind::LBracket) {
				Token lb = advance();
				Expr* idx = parseExpression();
				if(!idx || !expect(TokKind::RBracket))
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
				e = typed(m);
			} else if(k == TokKind::PlusPlus || k == TokKind::MinusMinus) {
				Token t = advance();
				ExprOp op = k == TokKind::PlusPlus ? ExprOp::PostInc : ExprOp::PostDec;
				e = makeUnary(t.offset, op, e);
			} else {
				break;
			}
			if(!e)
				return nullptr;
		}
		return e;
	}
} // namespace rat::cc
