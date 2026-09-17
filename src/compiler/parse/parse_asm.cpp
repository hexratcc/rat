#include "parse/parser.h"

namespace rat::cc {
	// string-literal...
	// adjacent literals concatenate
	B32 Parser::parseAsmTemplate(const String*& out) {
		if(!check(TokKind::StringLiteral)) {
			fail(peek(), "expected a string literal in 'asm'");
			return false;
		}
		String bytes;
		if(!parseStringLiteral(advance(), bytes))
			return false;
		while(check(TokKind::StringLiteral))
			if(!parseStringLiteral(advance(), bytes))
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
				if(!check(TokKind::Identifier)) {
					fail(peek(), "expected a symbolic operand name");
					return false;
				}
				op.name = arena.make<String>(lex.text(advance()));
				if(!expect(TokKind::RBracket, "']'"))
					return false;
			}
			if(!parseAsmTemplate(op.constraint))
				return false;
			if(!expect(TokKind::LParen, "'(' after an asm constraint"))
				return false;
			op.expr = parseExpression();
			if(!op.expr)
				return false;
			if(!expect(TokKind::RParen, "')'"))
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

	// asm [ volatile | inline | goto ]... ( template [: out [: in [: clobbers [: labels]]]] ) ;
	// labels: [ name [, name]... ]
	// no colon at all means basic asm, which is always volatile
	Stmt* Parser::parseAsmStatement() {
		Token kw = advance(); // asm
		AsmBlock* blk = arena.make<AsmBlock>();
		for(;;) {
			if(accept(TokKind::KwVolatile)) {
				blk->isVolatile = true;
				continue;
			}
			if(accept(TokKind::KwInline))
				continue;
			if(accept(TokKind::KwGoto)) {
				blk->isGoto = true;
				continue;
			}
			break;
		}
		if(!expect(TokKind::LParen, "'(' after 'asm'"))
			return nullptr;
		if(!parseAsmTemplate(blk->text))
			return nullptr;
		U32 section = 0;
		while(accept(TokKind::Colon)) {
			switch(section) {
			case 0:
				if(!parseAsmOperands(blk->outputs))
					return nullptr;
				break;
			case 1:
				if(!parseAsmOperands(blk->inputs))
					return nullptr;
				break;
			case 2:
				if(!parseAsmStrings(blk->clobbers))
					return nullptr;
				break;
			case 3:
				for(;;) {
					if(check(TokKind::RParen))
						break;
					if(!check(TokKind::Identifier)) {
						fail(peek(), "expected a label name in 'asm goto'");
						return nullptr;
					}
					blk->labels.push_back(arena.make<String>(lex.text(advance())));
					if(!accept(TokKind::Comma))
						break;
				}
				break;
			default:
				fail(peek(), "too many ':' sections in 'asm'");
				return nullptr;
			}
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
				if(!expect(TokKind::LParen, "'(' after an alias attribute"))
					return false;
				if(!parseAsmTemplate(aliasOut))
					return false;
				if(!expect(TokKind::RParen, "')'"))
					return false;
				continue;
			}
			if(check(TokKind::KwAsm)) {
				Token kw = advance();
				if(!expect(TokKind::LParen, "'(' after 'asm'"))
					return false;
				const String* label = nullptr;
				if(!parseAsmTemplate(label))
					return false;
				if(!expect(TokKind::RParen, "')'"))
					return false;
				fail(kw, "asm labels on declarations are not supported");
				return false;
			}
			if(accept(TokKind::KwNoinline)) {
				noInlineOut = true;
				continue;
			}
			if(check(TokKind::KwAlignas)) {
				if(!parseAlignasSpec(alignOut))
					return false;
				continue;
			}
			return true;
		}
	}
} // namespace rat::cc
