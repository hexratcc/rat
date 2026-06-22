#include "Parser.h"

#include "CharClass.h"

namespace rat::cc {
	namespace {
		struct BinInfo {
			I32 prec;
			ExprOp op;
		};

		BinInfo binInfo(TokKind kind) {
			switch (kind) {
			case TokKind::Star:
				return {10, ExprOp::Mul};
			case TokKind::Slash:
				return {10, ExprOp::Div};
			case TokKind::Percent:
				return {10, ExprOp::Rem};
			case TokKind::Plus:
				return {9, ExprOp::Add};
			case TokKind::Minus:
				return {9, ExprOp::Sub};
			case TokKind::Shl:
				return {8, ExprOp::Shl};
			case TokKind::Shr:
				return {8, ExprOp::Shr};
			case TokKind::Lt:
				return {7, ExprOp::Lt};
			case TokKind::Gt:
				return {7, ExprOp::Gt};
			case TokKind::Le:
				return {7, ExprOp::Le};
			case TokKind::Ge:
				return {7, ExprOp::Ge};
			case TokKind::EqEq:
				return {6, ExprOp::Eq};
			case TokKind::BangEq:
				return {6, ExprOp::Ne};
			case TokKind::Amp:
				return {5, ExprOp::BitAnd};
			case TokKind::Caret:
				return {4, ExprOp::BitXor};
			case TokKind::Pipe:
				return {3, ExprOp::BitOr};
			case TokKind::AmpAmp:
				return {2, ExprOp::LogAnd};
			case TokKind::PipePipe:
				return {1, ExprOp::LogOr};
			default:
				return {0, ExprOp::Add};
			}
		}

		B32 assignOp(TokKind kind, ExprOp& op) {
			switch (kind) {
			case TokKind::Assign:
				op = ExprOp::Assign;
				return true;
			case TokKind::PlusEq:
				op = ExprOp::AddAssign;
				return true;
			case TokKind::MinusEq:
				op = ExprOp::SubAssign;
				return true;
			case TokKind::StarEq:
				op = ExprOp::MulAssign;
				return true;
			case TokKind::SlashEq:
				op = ExprOp::DivAssign;
				return true;
			case TokKind::PercentEq:
				op = ExprOp::RemAssign;
				return true;
			case TokKind::ShlEq:
				op = ExprOp::ShlAssign;
				return true;
			case TokKind::ShrEq:
				op = ExprOp::ShrAssign;
				return true;
			case TokKind::AmpEq:
				op = ExprOp::AndAssign;
				return true;
			case TokKind::PipeEq:
				op = ExprOp::OrAssign;
				return true;
			case TokKind::CaretEq:
				op = ExprOp::XorAssign;
				return true;
			default:
				return false;
			}
		}

		void utf8Encode(String& out, U32 cp) {
			if (cp < 0x80) {
				out.push_back((char)cp);
			} else if (cp < 0x800) {
				out.push_back((char)(0xC0 | (cp >> 6)));
				out.push_back((char)(0x80 | (cp & 0x3F)));
			} else if (cp < 0x10000) {
				out.push_back((char)(0xE0 | (cp >> 12)));
				out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
				out.push_back((char)(0x80 | (cp & 0x3F)));
			} else {
				out.push_back((char)(0xF0 | (cp >> 18)));
				out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
				out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
				out.push_back((char)(0x80 | (cp & 0x3F)));
			}
		}

		B32 isTypeQualifier(TokKind kind) {
			switch (kind) {
			case TokKind::KwConst:
			case TokKind::KwVolatile:
			case TokKind::KwRestrict:
				return true;
			default:
				return false;
			}
		}

		B32 isQualOrStorage(TokKind kind) {
			switch (kind) {
			case TokKind::KwStatic:
			case TokKind::KwExtern:
			case TokKind::KwRegister:
			case TokKind::KwAuto:
			case TokKind::KwInline:
				return true;
			default:
				return isTypeQualifier(kind);
			}
		}

		B32 isTypeStart(TokKind kind) {
			switch (kind) {
			case TokKind::KwVoid:
			case TokKind::KwBool:
			case TokKind::KwChar:
			case TokKind::KwShort:
			case TokKind::KwInt:
			case TokKind::KwLong:
			case TokKind::KwFloat:
			case TokKind::KwDouble:
			case TokKind::KwSigned:
			case TokKind::KwUnsigned:
			case TokKind::KwComplex:
			case TokKind::KwImaginary:
			case TokKind::KwEnum:
			case TokKind::KwStruct:
			case TokKind::KwUnion:
			case TokKind::KwTypeof:
				return true;
			default:
				return isQualOrStorage(kind);
			}
		}

		B32 unaryOp(TokKind kind, ExprOp& op) {
			switch (kind) {
			case TokKind::Plus:
				op = ExprOp::Pos;
				return true;
			case TokKind::Minus:
				op = ExprOp::Neg;
				return true;
			case TokKind::Bang:
				op = ExprOp::Not;
				return true;
			case TokKind::Tilde:
				op = ExprOp::BitNot;
				return true;
			case TokKind::Amp:
				op = ExprOp::Addr;
				return true;
			case TokKind::Star:
				op = ExprOp::Deref;
				return true;
			case TokKind::KwReal:
				op = ExprOp::Real;
				return true;
			case TokKind::KwImag:
				op = ExprOp::Imag;
				return true;
			default:
				return false;
			}
		}

		String decodeUtf8ToUtf32LE(const String& bytes) {
			String w;
			U32 i = 0, n = (U32)bytes.size();
			while (i < n) {
				U8 b0 = (U8)bytes[i];
				U32 cp = 0, extra = 0;
				if (b0 < 0x80) {
					cp = b0;
				} else if ((b0 & 0xE0) == 0xC0) {
					cp = b0 & 0x1F;
					extra = 1;
				} else if ((b0 & 0xF0) == 0xE0) {
					cp = b0 & 0x0F;
					extra = 2;
				} else if ((b0 & 0xF8) == 0xF0) {
					cp = b0 & 0x07;
					extra = 3;
				} else {
					cp = b0;
				}
				++i;
				for (U32 k = 0; k < extra && i < n; ++k, ++i)
					cp = (cp << 6) | ((U8)bytes[i] & 0x3F);
				for (U32 k = 0; k < 4; ++k)
					w.push_back((char)(U8)(cp >> (8 * k)));
			}
			return w;
		}

	} // namespace

	Parser::Parser(Lexer& lexer, Arena& arena, const TargetInfo& target)
			: lex(lexer), arena(arena), target(target) {
		ArrayType* vl = arena.make<ArrayType>();
		vl->elem = CType{8, true, false, 0}; // unsigned char
		vl->count = target.getPointerSizeInBytes() * 4;
		CType vaList{8, true, false, 0};
		vaList.array = vl;
		typedefs["va_list"] = vaList;
		typedefs["__builtin_va_list"] = vaList;
	}

	void Parser::fail(const Token& at, const String& msg) {
		if (failed)
			return;
		std::ostringstream os;
		os << lex.file() << ":" << at.line << ":" << at.col << ": " << msg;
		errMsg = os.str();
		failed = true;
	}

	B32 Parser::accept(TokKind kind) {
		if (peek().kind == kind) {
			advance();
			return true;
		}
		return false;
	}

	B32 Parser::expect(TokKind kind, const char* what) {
		if (peek().kind == kind) {
			advance();
			return true;
		}
		fail(peek(), String("expected ") + what + ", found '" +
										 tokKindName(peek().kind) + "'");
		return false;
	}

	Expr* Parser::makeExpr(ExprKind kind, U32 offset) {
		Expr* e = arena.make<Expr>();
		e->kind = kind;
		e->offset = offset;
		return e;
	}

	Expr* Parser::makeInt(const Token& tok, I64 value, B32 isUnsigned,
												B32 isLong) {
		Expr* e = makeExpr(ExprKind::IntLit, tok.offset);
		e->intLit = {value, isUnsigned, isLong};
		return e;
	}

	Expr* Parser::makeIdent(const Token& tok) {
		Expr* e = makeExpr(ExprKind::Ident, tok.offset);
		e->ident.name = arena.make<String>(lex.text(tok));
		return e;
	}

	Expr* Parser::makeUnary(U32 offset, ExprOp op, Expr* operand) {
		Expr* e = makeExpr(ExprKind::Unary, offset);
		e->unary = {op, operand};
		return e;
	}

	Expr* Parser::makeBinary(U32 offset, ExprOp op, Expr* lhs, Expr* rhs) {
		Expr* e = makeExpr(ExprKind::Binary, offset);
		e->binary = {op, lhs, rhs};
		return e;
	}

	B32 Parser::parseIntLiteral(const Token& tok, I64& value, B32& isUnsigned,
															B32& isLong) {
		String s = lex.text(tok);
		isUnsigned = false;
		isLong = false;

		U32 end = (U32)s.size();
		while (end > 0) {
			char c = s[end - 1];
			if (c == 'u' || c == 'U') {
				isUnsigned = true;
				--end;
			} else if (c == 'l' || c == 'L') {
				isLong = true;
				--end;
			} else {
				break;
			}
		}

		I32 base = 10;
		U32 start = 0;
		if (end >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
			base = 16;
			start = 2;
		} else if (end >= 1 && s[0] == '0') {
			base = 8;
		}

		U64 v = 0;
		for (U32 i = start; i < end; ++i) {
			I32 d = hexVal(s[i]);
			if (d < 0 || d >= base) {
				fail(tok, "invalid digit in integer constant");
				return false;
			}
			v = v * (U64)base + (U64)d;
		}

		B32 dec = (base == 10);
		B32 fitsI32 = v <= 0x7fffffffULL;
		B32 fitsU32 = v <= 0xffffffffULL;
		B32 fitsI64 = v <= 0x7fffffffffffffffULL;
		if (isUnsigned) {
			// unsigned int, then unsigned long
			isLong = isLong || !fitsU32;
		} else if (isLong) {
			// long, then unsigned long
			if (!fitsI64)
				isUnsigned = true;
		} else if (dec) {
			if (!fitsI32)
				isLong = true;
		} else {
			// hex/octal: int, unsigned int, long, unsigned long
			if (fitsI32) {
				// int
			} else if (fitsU32) {
				isUnsigned = true; // unsigned int (32-bit)
			} else if (fitsI64) {
				isLong = true; // long
			} else {
				isLong = true;
				isUnsigned = true; // unsigned long
			}
		}

		value = (I64)v;
		return true;
	}

	static U32 escapeMaxVal(char prefix) {
		switch (prefix) {
		case 'u':
			return 0xFFFFu;
		case 'L':
		case 'U':
			return 0xFFFFFFFFu;
		default:
			return 0xFFu;
		}
	}

	B32 Parser::decodeEscape(const String& s, U32& i, U32 end, const Token& tok,
													 U32 maxVal, U8& out) {
		if (i >= end) {
			fail(tok, "unterminated escape");
			return false;
		}
		char e = s[i++];
		if (simpleEscape(e, out))
			return true;
		switch (e) {
		case '?':
			out = '?';
			return true;
		case 'x': {
			if (i >= end || !isHexDigit(s[i])) {
				fail(tok, "expected hex digits in escape");
				return false;
			}
			U32 hv = 0;
			while (i < end && isHexDigit(s[i]))
				hv = hv * 16 + (U32)hexVal(s[i++]);
			if (hv > maxVal) {
				fail(tok, "hex escape out of range");
				return false;
			}
			out = (U8)hv;
			return true;
		}
		default:
			if (isOctalDigit(e)) {
				// octal escape
				U32 ov = (U32)(e - '0');
				for (U32 k = 0; k < 2 && i < end && isOctalDigit(s[i]); ++k, ++i)
					ov = ov * 8 + (U32)(s[i] - '0');
				if (ov > maxVal) {
					fail(tok, "octal escape out of range");
					return false;
				}
				out = (U8)ov;
			} else {
				out = (U8)e; // unknown escape: take the character literally
			}
			return true;
		}
	}

	B32 Parser::decodeUcn(const String& s, U32& i, U32 end, const Token& tok,
												U32& cp) {
		char kind = s[i++]; // 'u' or 'U'
		U32 ndigits = (kind == 'u') ? 4 : 8;
		if (i + ndigits > end) {
			fail(tok, "incomplete universal character name");
			return false;
		}
		U32 v = 0;
		for (U32 k = 0; k < ndigits; ++k) {
			if (!isHexDigit(s[i + k])) {
				fail(tok, "universal character name requires hex digits");
				return false;
			}
			v = v * 16 + (U32)hexVal(s[i + k]);
		}
		i += ndigits;
		if ((v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF) {
			fail(tok, "invalid universal character name");
			return false;
		}
		cp = v;
		return true;
	}

	B32 Parser::parseCharLiteral(const Token& tok, I64& value) {
		String s = lex.text(tok);
		U32 maxVal = escapeMaxVal(s.size() ? s[0] : '\'');
		// Skip any encoding prefix
		U32 i = 0;
		while (i < s.size() && s[i] != '\'')
			++i;
		++i;
		U32 end = (U32)s.size();
		if (end > 0 && s[end - 1] == '\'')
			--end;

		if (i >= end) {
			fail(tok, "empty character constant");
			return false;
		}

		I64 v = 0;
		U32 count = 0;
		while (i < end) {
			I64 c;
			if (s[i] == '\\') {
				++i;
				if (i < end && (s[i] == 'u' || s[i] == 'U')) {
					U32 cp;
					if (!decodeUcn(s, i, end, tok, cp))
						return false;
					c = (I64)cp;
				} else {
					U8 byte;
					if (!decodeEscape(s, i, end, tok, maxVal, byte))
						return false;
					c = (I64)(I8)byte;
				}
			} else {
				c = (U8)s[i++];
			}
			if (count == 0)
				v = c;
			else
				v = (v << 8) | (c & 0xff);
			++count;
		}
		value = v;
		return true;
	}

	B32 Parser::parseStringLiteral(const Token& tok, String& out) {
		String s = lex.text(tok);
		U32 maxVal = (s.size() && s[0] == 'u' && s.size() > 1 && s[1] == '8')
										 ? 0xFFu
										 : escapeMaxVal(s.size() ? s[0] : '"');
		U32 i = 0;
		while (i < s.size() && s[i] != '"')
			++i;
		++i;
		U32 end = (U32)s.size();
		if (end > 0 && s[end - 1] == '"')
			--end;
		while (i < end) {
			char c = s[i++];
			if (c != '\\') {
				out.push_back(c);
				continue;
			}
			if (i < end && (s[i] == 'u' || s[i] == 'U')) {
				U32 cp;
				if (!decodeUcn(s, i, end, tok, cp))
					return false;
				utf8Encode(out, cp);
				continue;
			}
			U8 byte;
			if (!decodeEscape(s, i, end, tok, maxVal, byte))
				return false;
			out.push_back((char)byte);
		}
		return true;
	}

	Expr* Parser::parseBuiltinOffsetof(const Token& kw) {
		if (!expect(TokKind::LParen, "'('"))
			return nullptr;
		CType ty;
		if (!parseTypeSpec(ty)) {
			fail(peek(), "expected a type in __builtin_offsetof");
			return nullptr;
		}
		if (!expect(TokKind::Comma, "','"))
			return nullptr;
		// member-designator: identifier ('.' identifier | '[' const ']')*
		if (peek().kind != TokKind::Identifier) {
			fail(peek(), "expected a member name in __builtin_offsetof");
			return nullptr;
		}
		I64 off = 0;
		CType cur = ty;
		B32 first = true;
		for (;;) {
			if (first || accept(TokKind::Dot)) {
				first = false;
				if (peek().kind != TokKind::Identifier) {
					fail(peek(), "expected a member name in __builtin_offsetof");
					return nullptr;
				}
				Token m = advance();
				if (!isStruct(cur)) {
					fail(m, "__builtin_offsetof of a member of a non-struct type");
					return nullptr;
				}
				const Field* f = cur.strukt->find(lex.text(m));
				if (!f) {
					fail(m, "no such member in __builtin_offsetof");
					return nullptr;
				}
				off += f->offset;
				cur = f->type; // element type for array members
			} else if (accept(TokKind::LBracket)) {
				Expr* ix = parseConditional();
				I64 v = 0;
				if (!ix || !evalIntConst(ix, v)) {
					fail(peek(), "array index in __builtin_offsetof must be constant");
					return nullptr;
				}
				if (!expect(TokKind::RBracket, "']'"))
					return nullptr;
				off += v * (I64)typeSizeBytes(cur);
			} else {
				break;
			}
		}
		if (!expect(TokKind::RParen, "')'"))
			return nullptr;
		return makeInt(kw, off, true, true);
	}

	B32 Parser::parseTypeName(CType& out) {
		CType ty;
		if (!parseTypeSpec(ty))
			return false;
		parsePointers(ty);
		if (looksLikeFuncPtr()) { // abstract function-pointer/grouped type
			Token ignored;
			B32 hn = false;
			CType ft;
			if (!parseDeclaratorType(ty, ignored, hn, ft))
				return false;
			ty = ft;
		} else if (check(TokKind::LBracket)) { // array type-name: T[N] or T[]
			List<Dim> dims;
			while (accept(TokKind::LBracket)) {
				Dim d{0, nullptr};
				if (peek().kind != TokKind::RBracket) {
					Expr* len = parseConditional();
					if (!len)
						return false;
					I64 v = 0;
					if (tryEvalIntConst(len, v) && v > 0)
						d.count = (U32)v;
					else
						d.expr = len; // VLA
				}
				if (!expect(TokKind::RBracket, "']'"))
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
		if (!expect(TokKind::LParen, "'('"))
			return nullptr;
		Expr* control = parseAssignment();
		if (!control)
			return nullptr;
		Expr* e = makeExpr(ExprKind::Generic, kw.offset);
		e->generic.control = control;
		while (accept(TokKind::Comma)) {
			GenericAssoc assoc;
			if (peek().kind == TokKind::KwDefault) {
				advance();
				assoc.isDefault = true;
			} else {
				if (!parseTypeName(assoc.type))
					return nullptr;
			}
			if (!expect(TokKind::Colon, "':'"))
				return nullptr;
			Expr* result = parseAssignment();
			if (!result)
				return nullptr;
			assoc.result = result;
			e->assocs.push_back(assoc);
		}
		if (!expect(TokKind::RParen, "')'"))
			return nullptr;
		return e;
	}

	Expr* Parser::parsePrimary() {
		const Token& tok = peek();
		if (tok.kind == TokKind::KwGeneric)
			return parseGeneric();
		if (tok.kind == TokKind::StringLiteral) {
			Token first = advance();
			String head = lex.text(first);
			B32 wide = !head.empty() && head[0] == 'L';
			String bytes;
			if (!parseStringLiteral(first, bytes))
				return nullptr;
			while (peek().kind == TokKind::StringLiteral) {
				String h2 = lex.text(peek());
				if (!h2.empty() && h2[0] == 'L')
					wide = true;
				if (!parseStringLiteral(advance(), bytes))
					return nullptr;
			}
			Expr* e = makeExpr(ExprKind::StrLit, first.offset);
			if (wide) {
				String w = decodeUtf8ToUtf32LE(bytes);
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
		if (tok.kind == TokKind::CharConstant) {
			Token lit = advance();
			I64 value;
			if (!parseCharLiteral(lit, value))
				return nullptr;
			return makeInt(lit, value, false, false);
		}
		if (tok.kind == TokKind::IntConstant) {
			Token lit = advance();
			I64 value;
			B32 isUnsigned, isLong;
			if (!parseIntLiteral(lit, value, isUnsigned, isLong))
				return nullptr;
			return makeInt(lit, value, isUnsigned, isLong);
		}
		if (tok.kind == TokKind::FloatConstant) {
			Token lit = advance();
			String text = lex.text(lit);
			B32 isFloat = false, isLongDouble = false, isImaginary = false;
			while (!text.empty()) {
				char c = text.back();
				if (c == 'f' || c == 'F')
					isFloat = true;
				else if (c == 'l' || c == 'L')
					isLongDouble = true;
				else if (c == 'i' || c == 'I' || c == 'j' || c == 'J')
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
		if (tok.kind == TokKind::Identifier) {
			Token id = advance();
			// __func__
			if (lex.text(id) == "__func__") {
				Expr* e = makeExpr(ExprKind::StrLit, id.offset);
				e->str.bytes = arena.make<String>(curFuncName);
				return e;
			}
			// __builtin_offsetof(type, member)
			if (lex.text(id) == "__builtin_offsetof")
				return parseBuiltinOffsetof(id);
			auto ec = enumConstants.find(lex.text(id));
			if (ec != enumConstants.end())
				return makeInt(id, ec->second, false, false);
			return makeIdent(id);
		}
		if (tok.kind == TokKind::LParen) {
			advance();
			// GNU statement expression
			if (peek().kind == TokKind::LBrace) {
				Stmt* body = parseCompound();
				if (!body)
					return nullptr;
				if (!expect(TokKind::RParen, "')'"))
					return nullptr;
				Expr* e = makeExpr(ExprKind::StmtExpr, tok.offset);
				e->stmtExpr.body = body;
				return e;
			}
			Expr* inner = parseExpression();
			if (!inner)
				return nullptr;
			if (!expect(TokKind::RParen, "')'"))
				return nullptr;
			return inner;
		}
		fail(tok,
				 String("expected expression, found '") + tokKindName(tok.kind) + "'");
		return nullptr;
	}

	Expr* Parser::parsePostfix() {
		Expr* e = parsePrimary();
		if (!e)
			return nullptr;
		return parsePostfixTail(e);
	}

	Expr* Parser::parsePostfixTail(Expr* e) {
		for (;;) {
			TokKind k = peek().kind;
			if (k == TokKind::LParen) {
				// __builtin_va_arg(ap, type)
				if (e->kind == ExprKind::Ident &&
						*e->ident.name == "__builtin_va_arg") {
					Token lp = advance(); // '('
					Expr* ap = parseAssignment();
					if (!ap)
						return nullptr;
					if (!expect(TokKind::Comma, "','"))
						return nullptr;
					CType ty;
					if (!parseTypeSpec(ty)) {
						fail(peek(), "expected a type in __builtin_va_arg");
						return nullptr;
					}
					parsePointers(ty);
					if (!expect(TokKind::RParen, "')'"))
						return nullptr;
					Expr* va = makeExpr(ExprKind::VaArg, lp.offset);
					va->vaArg.ap = ap;
					va->vaArg.type = ty;
					e = va;
					continue;
				}
				Token lp = advance();
				Expr* callE = makeExpr(ExprKind::Call, lp.offset);
				if (e->kind == ExprKind::Ident) {
					// by-name call
					callE->call.callee = e->ident.name;
					callE->call.target = nullptr;
				} else {
					// indirect call
					callE->call.callee = nullptr;
					callE->call.target = e;
				}
				if (peek().kind != TokKind::RParen) {
					for (;;) {
						Expr* arg = parseAssignment();
						if (!arg)
							return nullptr;
						callE->args.push_back(arg);
						if (!accept(TokKind::Comma))
							break;
					}
				}
				if (!expect(TokKind::RParen, "')'"))
					return nullptr;
				e = callE;
			} else if (k == TokKind::LBracket) {
				Token lb = advance();
				Expr* idx = parseExpression();
				if (!idx)
					return nullptr;
				if (!expect(TokKind::RBracket, "']'"))
					return nullptr;
				Expr* sum = makeBinary(lb.offset, ExprOp::Add, e, idx);
				e = makeUnary(lb.offset, ExprOp::Deref, sum);
			} else if (k == TokKind::Dot || k == TokKind::Arrow) {
				Token t = advance();
				if (peek().kind != TokKind::Identifier) {
					fail(peek(), "expected member name after '" +
													 String(k == TokKind::Arrow ? "->" : ".") + "'");
					return nullptr;
				}
				Token nameTok = advance();
				Expr* m = makeExpr(ExprKind::Member, t.offset);
				m->member.base = e;
				m->member.name = arena.make<String>(lex.text(nameTok));
				m->member.arrow = k == TokKind::Arrow;
				e = m;
			} else if (k == TokKind::PlusPlus || k == TokKind::MinusMinus) {
				Token t = advance();
				ExprOp op = k == TokKind::PlusPlus ? ExprOp::PostInc : ExprOp::PostDec;
				e = makeUnary(t.offset, op, e);
			} else {
				break;
			}
		}
		return e;
	}

	Expr* Parser::parseUnary() {
		if (peek().kind == TokKind::KwSizeof) {
			Token kw = advance(); // sizeof
			Expr* e = makeExpr(ExprKind::Sizeof, kw.offset);
			if (peek().kind == TokKind::LParen && startsType(peek2())) {
				advance(); // (
				CType ty;
				if (!parseTypeName(ty)) // sizeof(typename)
					return nullptr;
				if (!expect(TokKind::RParen, "')'"))
					return nullptr;
				e->sizeOf.type = ty;
				e->sizeOf.operand = nullptr;
			} else {
				Expr* operand = parseUnary();
				if (!operand)
					return nullptr;
				e->sizeOf.operand = operand;
			}
			return e;
		}
		if (peek().kind == TokKind::LParen && startsType(peek2())) {
			Token lp = advance(); // (
			CType ty;
			if (!parseTypeSpec(ty))
				return nullptr;
			parsePointers(ty);
			if (looksLikeFuncPtr()) { // cast to a function-pointer/grouped type
				Token ignored;
				B32 hn = false;
				CType ft;
				if (!parseDeclaratorType(ty, ignored, hn, ft))
					return nullptr;
				ty = ft;
				if (!expect(TokKind::RParen, "')'"))
					return nullptr;
				Expr* operand = parseUnary();
				if (!operand)
					return nullptr;
				Expr* e = makeExpr(ExprKind::Cast, lp.offset);
				e->cast.type = ty;
				e->cast.operand = operand;
				return e;
			}
			B32 isArr = false;
			Expr* arrLen = nullptr;
			if (accept(TokKind::LBracket)) { // array type-name: (T[]) or (T[N])
				isArr = true;
				if (peek().kind != TokKind::RBracket) {
					arrLen = parseConditional();
					if (!arrLen)
						return nullptr;
				}
				if (!expect(TokKind::RBracket, "']'"))
					return nullptr;
			}
			if (!expect(TokKind::RParen, "')'"))
				return nullptr;
			if (peek().kind == TokKind::LBrace) { // compound literal
				Expr* init = parseInitializer();
				if (!init)
					return nullptr;
				Expr* e = makeExpr(ExprKind::CompoundLit, lp.offset);
				e->compound.type = ty;
				e->compound.init = init;
				e->compound.arrayLen = arrLen;
				e->compound.isArray = isArr;
				return parsePostfixTail(e); // allow [..], .x, etc. after the literal
			}
			if (isArr) {
				fail(lp, "array types may not be used in a cast");
				return nullptr;
			}
			Expr* operand = parseUnary();
			if (!operand)
				return nullptr;
			Expr* e = makeExpr(ExprKind::Cast, lp.offset);
			e->cast.type = ty;
			e->cast.operand = operand;
			return e;
		}
		ExprOp op;
		if (unaryOp(peek().kind, op)) {
			Token t = advance();
			Expr* operand = parseUnary();
			if (!operand)
				return nullptr;
			return makeUnary(t.offset, op, operand);
		}
		return parsePrimary();
	}

	Expr* Parser::parseBinary(I32 minPrec) {
		Expr* lhs = parseUnary();
		if (!lhs)
			return nullptr;
		for (;;) {
			BinInfo info = binInfo(peek().kind);
			if (info.prec == 0 || info.prec < minPrec)
				break;
			Token opTok = advance();
			Expr* rhs = parseBinary(info.prec + 1); // left-associative
			if (!rhs)
				return nullptr;
			lhs = makeBinary(opTok.offset, info.op, lhs, rhs);
		}
		return lhs;
	}

	Expr* Parser::parseConditional() {
		Expr* cond = parseBinary(1);
		if (!cond)
			return nullptr;
		if (peek().kind != TokKind::Question)
			return cond;
		Token q = advance();
		Expr* whenTrue = parseExpression();
		if (!whenTrue)
			return nullptr;
		if (!expect(TokKind::Colon, "':'"))
			return nullptr;
		Expr* whenFalse = parseConditional();
		if (!whenFalse)
			return nullptr;
		Expr* e = makeExpr(ExprKind::Ternary, q.offset);
		e->ternary = {cond, whenTrue, whenFalse};
		return e;
	}

	Expr* Parser::parseAssignment() {
		Expr* lhs = parseConditional();
		if (!lhs)
			return nullptr;
		ExprOp op;
		if (assignOp(peek().kind, op)) {
			Token opTok = advance();
			Expr* rhs = parseAssignment(); // right-associative
			if (!rhs)
				return nullptr;
			return makeBinary(opTok.offset, op, lhs, rhs);
		}
		return lhs;
	}

	Expr* Parser::parseExpression() {
		Expr* e = parseAssignment();
		if (!e)
			return nullptr;
		while (peek().kind == TokKind::Comma) {
			Token c = advance();
			Expr* rhs = parseAssignment();
			if (!rhs)
				return nullptr;
			Expr* comma = makeExpr(ExprKind::Comma, c.offset);
			comma->comma = {e, rhs};
			e = comma;
		}
		return e;
	}

	Stmt* Parser::parseDeclaration() {
		Token start = peek();
		if (!expect(TokKind::KwInt, "'int'"))
			return nullptr;
		Stmt* s = arena.make<Stmt>();
		s->kind = StmtKind::Decl;
		s->offset = start.offset;
		for (;;) {
			if (peek().kind != TokKind::Identifier) {
				fail(peek(), "expected declarator name");
				return nullptr;
			}
			Token nameTok = advance();
			Declarator d;
			d.name = arena.make<String>(lex.text(nameTok));
			d.offset = nameTok.offset;
			if (accept(TokKind::Assign)) {
				d.init = parseAssignment();
				if (!d.init)
					return nullptr;
			}
			s->decls.push_back(d);
			if (!accept(TokKind::Comma))
				break;
		}
		if (!expect(TokKind::Semicolon, "';'"))
			return nullptr;
		return s;
	}

	Stmt* Parser::parseStatement() {
		const Token& tok = peek();
		if (tok.kind == TokKind::LBrace)
			return parseCompound();

		if (tok.kind == TokKind::KwInt)
			return parseDeclaration();

		if (tok.kind == TokKind::KwReturn) {
			Token kw = advance();
			Stmt* s = arena.make<Stmt>();
			s->kind = StmtKind::Return;
			s->offset = kw.offset;
			if (peek().kind != TokKind::Semicolon) {
				s->expr = parseExpression();
				if (!s->expr)
					return nullptr;
			}
			if (!expect(TokKind::Semicolon, "';'"))
				return nullptr;
			return s;
		}

		if (tok.kind == TokKind::Semicolon) {
			Token semi = advance();
			Stmt* s = arena.make<Stmt>();
			s->kind = StmtKind::Empty;
			s->offset = semi.offset;
			return s;
		}

		Stmt* s = arena.make<Stmt>();
		s->kind = StmtKind::Expr;
		s->offset = tok.offset;
		s->expr = parseExpression();
		if (!s->expr)
			return nullptr;
		if (!expect(TokKind::Semicolon, "';'"))
			return nullptr;
		return s;
	}

	Stmt* Parser::parseCompound() {
		Token open = peek();
		if (!expect(TokKind::LBrace, "'{'"))
			return nullptr;
		Stmt* block = arena.make<Stmt>();
		block->kind = StmtKind::Compound;
		block->offset = open.offset;
		while (!failed && peek().kind != TokKind::RBrace &&
					 peek().kind != TokKind::Eof) {
			Stmt* s = parseStatement();
			if (!s)
				return nullptr;
			block->body.push_back(s);
		}
		if (!expect(TokKind::RBrace, "'}'"))
			return nullptr;
		return block;
	}

	FuncDef* Parser::parseFunction() {
		Token start = peek();
		if (!expect(TokKind::KwInt, "'int'"))
			return nullptr;
		if (peek().kind != TokKind::Identifier) {
			fail(peek(), "expected function name");
			return nullptr;
		}
		Token nameTok = advance();
		if (!expect(TokKind::LParen, "'('"))
			return nullptr;
		if (peek().kind == TokKind::KwVoid)
			advance();
		if (!expect(TokKind::RParen, "')'"))
			return nullptr;

		FuncDef* fn = arena.make<FuncDef>();
		fn->name = lex.text(nameTok);
		fn->retType = TypeSpec::Int;
		fn->offset = start.offset;
		fn->body = parseCompound();
		if (!fn->body)
			return nullptr;
		return fn;
	}

	TransUnit* Parser::parseUnit() {
		TransUnit* unit = arena.make<TransUnit>();
		while (!failed && peek().kind != TokKind::Eof) {
			FuncDef* fn = parseFunction();
			if (!fn)
				return nullptr;
			unit->functions.push_back(fn);
		}
		if (failed)
			return nullptr;
		return unit;
	}
} // namespace rat::cc
