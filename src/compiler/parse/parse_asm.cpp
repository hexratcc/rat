#include "parse/parser.h"

namespace rat::cc {
	// string-literal...
	// adjacent literals concatenate
	B32 Parser::parseStrings(String& out, const C8* where) {
		if(!check(TokKind::StringLiteral))
			return fail(peek(), String("expected a string literal in ") + where);
		while(check(TokKind::StringLiteral))
			if(!parseStringLiteral(advance(), out))
				return false;
		return true;
	}

	// string-literal...
	B32 Parser::parseAsmTemplate(const String*& out) {
		String bytes;
		if(!parseStrings(bytes, "'asm'"))
			return false;
		out = arena.make<String>(std::move(bytes));
		return true;
	}

	// [ operand [, operand]... ]
	// operand: [ '[' name ']' ] string-literal... ( expr )
	B32 Parser::parseAsmOperands(List<AsmOperand>& out) {
		if(check(TokKind::Colon) || check(TokKind::RParen))
			return true;
		for(;;) {
			AsmOperand op;
			if(accept(TokKind::LBracket)) {
				if(!check(TokKind::Identifier))
					return fail(peek(), "expected a symbolic operand name");
				op.name = arena.make<String>(lex.text(advance()));
				if(!expect(TokKind::RBracket, "']'"))
					return false;
			}
			if(!parseAsmTemplate(op.constraint))
				return false;
			if(!expect(TokKind::LParen, "'(' after an asm constraint"))
				return false;
			op.expr = parseExpression();
			if(!op.expr || !expect(TokKind::RParen, "')'"))
				return false;
			out.push_back(op);
			if(!accept(TokKind::Comma))
				break;
		}
		return true;
	}

	// [ string-literal... [, string-literal...]... ]
	B32 Parser::parseAsmStrings(List<const String*>& out) {
		if(check(TokKind::Colon) || check(TokKind::RParen))
			return true;
		for(;;) {
			const String* s = nullptr;
			if(!parseAsmTemplate(s))
				return false;
			out.push_back(s);
			if(!accept(TokKind::Comma))
				break;
		}
		return true;
	}

	// [ name [, name]... ]
	B32 Parser::parseAsmLabels(List<const String*>& out) {
		while(!check(TokKind::RParen)) {
			if(!check(TokKind::Identifier))
				return fail(peek(), "expected a label name in 'asm goto'");
			out.push_back(arena.make<String>(lex.text(advance())));
			if(!accept(TokKind::Comma))
				break;
		}
		return true;
	}

	// asm [ volatile | inline | goto ]... ( template [: out [: in [: clobbers [: labels]]]] ) ;
	// no colon at all means basic asm, which is always volatile
	Stmt* Parser::parseAsmStatement() {
		Token kw = advance(); // asm
		AsmBlock* blk = arena.make<AsmBlock>();
		for(;;) {
			if(accept(TokKind::KwVolatile))
				blk->isVolatile = true;
			else if(accept(TokKind::KwGoto))
				blk->isGoto = true;
			else if(!accept(TokKind::KwInline))
				break;
		}
		if(!expect(TokKind::LParen, "'(' after 'asm'") || !parseAsmTemplate(blk->text))
			return nullptr;
		U32 section = 0;
		while(accept(TokKind::Colon)) {
			B32 parsed;
			if(section == 0)
				parsed = parseAsmOperands(blk->outputs);
			else if(section == 1)
				parsed = parseAsmOperands(blk->inputs);
			else if(section == 2)
				parsed = parseAsmStrings(blk->clobbers);
			else if(section == 3)
				parsed = parseAsmLabels(blk->labels);
			else
				return fail(peek(), "too many ':' sections in 'asm'");
			if(!parsed)
				return nullptr;
			++section;
		}
		if(section == 0)
			blk->isVolatile = true;
		if(!expect(TokKind::RParen, "')' closing 'asm'"))
			return nullptr;
		expect(TokKind::Semicolon, "';' after an 'asm' statement");
		Stmt* s = makeStmt(StmtKind::Asm, kw.offset);
		s->asmBlock = blk;
		return s;
	}

	// [ alias ( string-literal... ) | asm ( string-literal... ) | noinline | alignas ]...
	// the markers the preprocessor leaves behind, in any order; an asm label is rejected
	B32 Parser::parseDeclAttributes(const String*& aliasOut, B32& noInlineOut, U32& alignOut) {
		for(;;) {
			if(accept(TokKind::KwAlias)) {
				if(!expect(TokKind::LParen, "'(' after an alias attribute") || !parseAsmTemplate(aliasOut))
					return false;
				if(!expect(TokKind::RParen, "')'"))
					return false;
			} else if(check(TokKind::KwAsm)) {
				Token kw = advance();
				const String* label = nullptr;
				if(!expect(TokKind::LParen, "'(' after 'asm'") || !parseAsmTemplate(label))
					return false;
				if(!expect(TokKind::RParen, "')'"))
					return false;
				return fail(kw, "asm labels on declarations are not supported");
			} else if(accept(TokKind::KwNoinline)) {
				noInlineOut = true;
			} else if(check(TokKind::KwAlignas)) {
				if(!parseAlignasSpec(alignOut))
					return false;
			} else {
				return true;
			}
		}
	}
} // namespace rat::cc
