#include "Parser.h"

#include "CharClass.h"

namespace rat::cc {
	namespace {
		constexpr I64 kIntMin = -2147483648LL;
		constexpr I64 kIntMax = 2147483647LL;

		struct BinInfo {
			I32 prec;
			ExprOp op;
		};

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

		void utf8Encode(String& out, U32 cp) {
			if(cp < 0x80) {
				out.push_back((char)cp);
			} else if(cp < 0x800) {
				out.push_back((char)(0xC0 | (cp >> 6)));
				out.push_back((char)(0x80 | (cp & 0x3F)));
			} else if(cp < 0x10000) {
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
			switch(kind) {
			case TokKind::KwConst:
			case TokKind::KwVolatile:
			case TokKind::KwRestrict:
				return true;
			default:
				return false;
			}
		}

		B32 isQualOrStorage(TokKind kind) {
			switch(kind) {
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
			switch(kind) {
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

	} // namespace

	Parser::Parser(Lexer& lexer, Arena& arena, const TargetInfo& target)
	: lex(lexer),
		arena(arena),
		target(target) {
		ArrayType* vl = arena.make<ArrayType>();
		vl->elem = CType{8, true, false, 0}; // unsigned char
		vl->count = target.getPointerSizeInBytes() * 4;
		CType vaList{8, true, false, 0};
		vaList.array = vl;
		typedefs["va_list"] = vaList;
		typedefs["__builtin_va_list"] = vaList;
	}

	void Parser::fail(const Token& at, const String& msg) {
		if(failed)
			return;
		std::ostringstream os;
		os << lex.file() << ":" << at.line << ":" << at.col << ": " << msg;
		errMsg = os.str();
		failed = true;
	}

	B32 Parser::accept(TokKind kind) {
		if(peek().kind == kind) {
			advance();
			return true;
		}
		return false;
	}

	B32 Parser::expect(TokKind kind, const char* what) {
		if(peek().kind == kind) {
			advance();
			return true;
		}
		fail(peek(), String("expected ") + what + ", found '" + tokKindName(peek().kind) + "'");
		return false;
	}

	Expr* Parser::makeExpr(ExprKind kind, U32 offset) {
		Expr* e = arena.make<Expr>();
		e->kind = kind;
		e->offset = offset;
		return e;
	}

	Stmt* Parser::makeStmt(StmtKind kind, U32 offset) {
		Stmt* s = arena.make<Stmt>();
		s->kind = kind;
		s->offset = offset;
		return s;
	}

	Expr* Parser::makeInt(const Token& tok, I64 value, B32 isUnsigned, B32 isLong) {
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

	B32 Parser::parseIntLiteral(const Token& tok, I64& value, B32& isUnsigned, B32& isLong) {
		String s = lex.text(tok);
		isUnsigned = false;
		isLong = false;

		U32 end = (U32)s.size();
		while(end > 0) {
			char c = s[end - 1];
			if(c == 'u' || c == 'U') {
				isUnsigned = true;
				--end;
			} else if(c == 'l' || c == 'L') {
				isLong = true;
				--end;
			} else {
				break;
			}
		}

		I32 base = 10;
		U32 start = 0;
		if(end >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
			base = 16;
			start = 2;
		} else if(end >= 1 && s[0] == '0') {
			base = 8;
		}

		U64 v = 0;
		for(U32 i = start; i < end; ++i) {
			I32 d = hexVal(s[i]);
			if(d < 0 || d >= base) {
				fail(tok, "invalid digit in integer constant");
				return false;
			}
			v = v * (U64)base + (U64)d;
		}

		B32 dec = (base == 10);
		B32 fitsI32 = v <= 0x7fffffffULL;
		B32 fitsU32 = v <= 0xffffffffULL;
		B32 fitsI64 = v <= 0x7fffffffffffffffULL;
		if(isUnsigned) {
			// unsigned int, then unsigned long
			isLong = isLong || !fitsU32;
		} else if(isLong) {
			// long, then unsigned long
			if(!fitsI64)
				isUnsigned = true;
		} else if(dec) {
			if(!fitsI32)
				isLong = true;
		} else {
			// hex/octal: int, unsigned int, long, unsigned long
			if(fitsI32) {
				// int
			} else if(fitsU32) {
				isUnsigned = true; // unsigned int (32-bit)
			} else if(fitsI64) {
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
		switch(prefix) {
		case 'u':
			return 0xFFFFu;
		case 'L':
		case 'U':
			return 0xFFFFFFFFu;
		default:
			return 0xFFu;
		}
	}

	B32 Parser::decodeEscape(
			const String& s, U32& i, U32 end, const Token& tok, U32 maxVal, U8& out) {
		if(i >= end) {
			fail(tok, "unterminated escape");
			return false;
		}
		char e = s[i++];
		if(simpleEscape(e, out))
			return true;
		switch(e) {
		case '?':
			out = '?';
			return true;
		case 'x': {
			if(i >= end || !isHexDigit(s[i])) {
				fail(tok, "expected hex digits in escape");
				return false;
			}
			U32 hv = 0;
			while(i < end && isHexDigit(s[i]))
				hv = hv * 16 + (U32)hexVal(s[i++]);
			if(hv > maxVal) {
				fail(tok, "hex escape out of range");
				return false;
			}
			out = (U8)hv;
			return true;
		}
		default:
			if(isOctalDigit(e)) {
				// octal escape
				U32 ov = (U32)(e - '0');
				for(U32 k = 0; k < 2 && i < end && isOctalDigit(s[i]); ++k, ++i)
					ov = ov * 8 + (U32)(s[i] - '0');
				if(ov > maxVal) {
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

	B32 Parser::decodeUcn(const String& s, U32& i, U32 end, const Token& tok, U32& cp) {
		char kind = s[i++]; // 'u' or 'U'
		U32 ndigits = (kind == 'u') ? 4 : 8;
		if(i + ndigits > end) {
			fail(tok, "incomplete universal character name");
			return false;
		}
		U32 v = 0;
		for(U32 k = 0; k < ndigits; ++k) {
			if(!isHexDigit(s[i + k])) {
				fail(tok, "universal character name requires hex digits");
				return false;
			}
			v = v * 16 + (U32)hexVal(s[i + k]);
		}
		i += ndigits;
		if((v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF) {
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
		while(i < s.size() && s[i] != '\'')
			++i;
		++i;
		U32 end = (U32)s.size();
		if(end > 0 && s[end - 1] == '\'')
			--end;

		if(i >= end) {
			fail(tok, "empty character constant");
			return false;
		}

		I64 v = 0;
		U32 count = 0;
		while(i < end) {
			I64 c;
			if(s[i] == '\\') {
				++i;
				if(i < end && (s[i] == 'u' || s[i] == 'U')) {
					U32 cp;
					if(!decodeUcn(s, i, end, tok, cp))
						return false;
					c = (I64)cp;
				} else {
					U8 byte;
					if(!decodeEscape(s, i, end, tok, maxVal, byte))
						return false;
					c = (I64)(I8)byte;
				}
			} else {
				c = (U8)s[i++];
			}
			if(count == 0)
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
		while(i < s.size() && s[i] != '"')
			++i;
		++i;
		U32 end = (U32)s.size();
		if(end > 0 && s[end - 1] == '"')
			--end;
		while(i < end) {
			char c = s[i++];
			if(c != '\\') {
				out.push_back(c);
				continue;
			}
			if(i < end && (s[i] == 'u' || s[i] == 'U')) {
				U32 cp;
				if(!decodeUcn(s, i, end, tok, cp))
					return false;
				utf8Encode(out, cp);
				continue;
			}
			U8 byte;
			if(!decodeEscape(s, i, end, tok, maxVal, byte))
				return false;
			out.push_back((char)byte);
		}
		return true;
	}

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
		if(unaryOp(peek().kind, op)) {
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
			BinInfo info = binInfo(peek().kind);
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
		if(assignOp(peek().kind, op)) {
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

	namespace {
		U32 alignUp(U32 value, U32 align) {
			return align <= 1 ? value : (value + align - 1) / align * align;
		}
	} // namespace

	StructType* Parser::complexStruct(CType realType) {
		auto it = complexLayouts.find(realType.bits);
		if(it != complexLayouts.end())
			return it->second;
		U32 elemBytes = (realType.bits + 7) / 8;
		CType elem = realType;
		elem.isComplex = false;
		StructType* st = arena.make<StructType>();
		st->size = 2 * elemBytes;
		st->align = elemBytes;
		st->isUnion = false;
		st->complete = true;
		Field re;
		re.name = arena.make<String>("__real");
		re.type = elem;
		re.offset = 0;
		Field im;
		im.name = arena.make<String>("__imag");
		im.type = elem;
		im.offset = elemBytes;
		st->fields.push_back(re);
		st->fields.push_back(im);
		complexLayouts[realType.bits] = st;
		return st;
	}

	B32 Parser::parseStructBody(StructType* st, B32 isUnion) {
		U32 offset = 0;
		U32 align = 1;
		U32 bitUnitOffset = 0;
		U32 bitUnitBytes = 0;
		U32 bitUnitUsed = 0;
		while(peek().kind != TokKind::RBrace && peek().kind != TokKind::Eof) {
			CType base;
			if(!parseTypeSpec(base)) {
				fail(peek(), "expected member type");
				return false;
			}
			if(peek().kind == TokKind::Semicolon && isStruct(base)) {
				advance();
				const StructType* inner = base.strukt;
				U32 mAlign = inner->align;
				U32 mbase = isUnion ? 0 : alignUp(offset, mAlign);
				B32 first = true;
				for(const Field& sub : inner->fields) {
					Field f = sub;
					f.offset = isUnion ? sub.offset : mbase + sub.offset;
					f.anonMember = true;
					f.anonFirst = first;
					f.anonUnion = inner->isUnion;
					first = false;
					st->fields.push_back(f);
				}
				U32 mEnd = mbase + inner->size;
				if(isUnion) {
					if(inner->size > offset)
						offset = inner->size;
				} else {
					offset = mEnd;
				}
				if(mAlign > align)
					align = mAlign;
				bitUnitBytes = 0;
				continue;
			}
			for(;;) {
				CType ft = base;
				parsePointers(ft);
				Token nameTok;
				B32 haveName = false;
				B32 isArr = false;
				B32 flexible = false;
				U32 count = 0;
				if(looksLikeFuncPtr()) {
					CType fpt;
					if(!parseFuncPtrDeclarator(ft, nameTok, fpt))
						return false;
					ft = fpt;
					haveName = true;
				} else {
					if(peek().kind == TokKind::Identifier) {
						nameTok = advance();
						haveName = true;
					}
					if(peek().kind == TokKind::LBracket) {
						Declarator d;
						d.type = ft;
						if(!parseArraySuffix(d))
							return false;
						ft = d.type;
						isArr = true;
						I64 n = 0;
						if(d.arrayLen) {
							if(!evalIntConst(d.arrayLen, n) || n < 0) {
								fail(nameTok, "array member size must be a constant");
								return false;
							}
						} else {
							flexible = true; // type name[] with no bound
						}
						count = (U32)n; // 0 == flexible array member
					}
				}
				// optional bitfield : width
				if(accept(TokKind::Colon)) {
					if(isArr || ft.ptr != 0 || ft.isFloat || ft.isComplex || ft.isVoid || isStruct(ft) ||
						 ft.func != nullptr) {
						fail(nameTok, "bit-field has invalid type");
						return false;
					}
					Expr* wE = parseConditional();
					if(!wE)
						return false;
					I64 w = 0;
					if(!evalIntConst(wE, w) || w < 0) {
						fail(peek(), "bitfield width must be a non-negative constant");
						return false;
					}
					U32 unitBytes = fieldByteSize(ft);
					if(unitBytes == 0)
						unitBytes = 4;
					U32 falign = fieldAlign(ft);
					if(w == 0 || bitUnitBytes != unitBytes || bitUnitUsed + (U32)w > unitBytes * 8) {
						bitUnitOffset = isUnion ? 0 : alignUp(offset, falign);
						bitUnitBytes = unitBytes;
						bitUnitUsed = 0;
						if(!isUnion)
							offset = bitUnitOffset + unitBytes;
						else if(unitBytes > offset)
							offset = unitBytes;
					}
					if(w > 0 && haveName) {
						Field f;
						f.name = arena.make<String>(lex.text(nameTok));
						f.type = ft;
						f.isBitfield = true;
						f.bitWidth = (U32)w;
						f.bitOffset = bitUnitUsed;
						f.offset = bitUnitOffset;
						st->fields.push_back(f);
					}
					bitUnitUsed += (U32)w;
					if(falign > align)
						align = falign;
					if(!accept(TokKind::Comma))
						break;
					continue;
				}
				bitUnitBytes = 0;
				if(!haveName) {
					fail(peek(), "expected member name");
					return false;
				}
				if(isStruct(ft) && !ft.strukt->complete) {
					fail(nameTok, "member has incomplete struct type");
					return false;
				}
				if(flexible) {
					if(isUnion) {
						fail(nameTok, "flexible array member not allowed in union");
						return false;
					}
					if(st->fields.empty()) {
						fail(nameTok, "flexible array member in struct with no other members");
						return false;
					}
					if(peek().kind == TokKind::Comma ||
						 !(peek().kind == TokKind::Semicolon && peek2().kind == TokKind::RBrace)) {
						fail(nameTok, "flexible array member must be the last member");
						return false;
					}
				}
				U32 esize = fieldByteSize(ft);
				U32 falign = fieldAlign(ft);
				U32 fsize = isArr ? esize * count : esize;
				Field f;
				f.name = arena.make<String>(lex.text(nameTok));
				f.type = ft;
				f.isArray = isArr;
				f.count = count;
				f.offset = isUnion ? 0 : alignUp(offset, falign);
				st->fields.push_back(f);
				if(isUnion) {
					if(fsize > offset)
						offset = fsize;
				} else {
					offset = f.offset + fsize;
				}
				if(falign > align)
					align = falign;
				if(!accept(TokKind::Comma))
					break;
			}
			if(!expect(TokKind::Semicolon, "';'"))
				return false;
		}
		if(!expect(TokKind::RBrace, "'}'"))
			return false;
		st->align = align;
		st->size = alignUp(offset, align);
		st->complete = true;
		return true;
	}

	B32 Parser::parseStructSpec(CType& out) {
		B32 isUnion = peek().kind == TokKind::KwUnion;
		advance(); // 'struct' or 'union'

		const String* tag = nullptr;
		if(peek().kind == TokKind::Identifier)
			tag = arena.make<String>(lex.text(advance()));

		StructType* st = nullptr;
		if(tag) {
			auto it = structTypes.find(*tag);
			if(it != structTypes.end())
				st = it->second;
			else {
				st = arena.make<StructType>();
				st->tag = *tag;
				st->isUnion = isUnion;
				structTypes[*tag] = st;
			}
		}

		if(peek().kind == TokKind::LBrace) {
			advance();
			if(!st)
				st = arena.make<StructType>(); // anon aggregate
			st->isUnion = isUnion;
			if(!parseStructBody(st, isUnion))
				return false;
		}

		if(!st) {
			fail(peek(), "use of undeclared struct/union tag");
			return false;
		}
		CType t;
		t.strukt = st;
		out = t;
		return true;
	}

	B32 Parser::parseTypeofSpec(CType& out) {
		advance(); // typeof / __typeof / __typeof__
		if(!expect(TokKind::LParen, "'('"))
			return false;
		if(startsType(peek())) {
			if(!parseTypeName(out))
				return false;
		} else {
			Expr* e = parseExpression();
			if(!e)
				return false;
			out = CType{};
			out.typeofExpr = e;
		}
		return expect(TokKind::RParen, "')'");
	}

	B32 Parser::parseEnumSpec(CType& out) {
		advance(); // enum
		if(peek().kind == TokKind::Identifier)
			advance();

		B32 anyNegative = false;
		if(peek().kind == TokKind::LBrace) {
			advance();
			I64 next = 0;
			while(peek().kind != TokKind::RBrace && peek().kind != TokKind::Eof) {
				if(peek().kind != TokKind::Identifier) {
					fail(peek(), "expected enumerator name");
					return false;
				}
				Token name = advance();
				I64 value = next;
				if(accept(TokKind::Assign)) {
					Expr* init = parseConditional();
					if(!init)
						return false;
					if(!evalIntConst(init, value))
						return false;
				}
				if(value < kIntMin || value > kIntMax) {
					fail(name, "enumerator value is not representable as int");
					return false;
				}
				if(value < 0)
					anyNegative = true;
				enumConstants[lex.text(name)] = value;
				next = value + 1;
				if(!accept(TokKind::Comma))
					break;
			}
			if(!expect(TokKind::RBrace, "'}'"))
				return false;
		}
		out = ctInt();
		if(!anyNegative)
			out.isUnsigned = true;
		return true;
	}

	B32 Parser::startsType(const Token& tok) {
		if(isTypeStart(tok.kind))
			return true;
		return tok.kind == TokKind::Identifier && typedefs.count(lex.text(tok)) != 0;
	}

	B32 Parser::parseTypedef() {
		advance(); // typedef
		CType base;
		if(!parseTypeSpec(base)) {
			fail(peek(), "expected type in typedef declaration");
			return false;
		}
		if(peek().kind == TokKind::Semicolon) {
			advance();
			return true;
		}
		for(;;) {
			Token nameTok;
			B32 haveName = false;
			CType t;
			if(!parseDeclaratorType(base, nameTok, haveName, t))
				return false;
			if(!haveName) {
				fail(peek(), "expected typedef name");
				return false;
			}
			typedefs[lex.text(nameTok)] = t;
			if(!accept(TokKind::Comma))
				break;
		}
		return expect(TokKind::Semicolon, "';'");
	}

	void Parser::parsePointers(CType& t) {
		for(;;) {
			B32 sawConst = false;
			while(isTypeQualifier(peek().kind)) {
				if(peek().kind == TokKind::KwConst)
					sawConst = true;
				advance();
			}
			if(sawConst && t.ptr < 32)
				t.quals |= (1u << t.ptr);
			if(!accept(TokKind::Star))
				break;
			++t.ptr;
		}
	}

	B32 Parser::parseTypeSpec(CType& out) {
		B32 isStatic = false;
		B32 isExtern = false;
		B32 isInline = false;
		B32 isConst = false;
		I32 storageCount = 0;
		sawStatic = false;
		sawExtern = false;
		sawInline = false;
		auto applyQualStorage = [&](TokKind sk) {
			if(sk == TokKind::KwStatic)
				isStatic = true;
			if(sk == TokKind::KwExtern)
				isExtern = true;
			if(sk == TokKind::KwInline)
				isInline = true;
			if(sk == TokKind::KwConst)
				isConst = true;
			if(sk == TokKind::KwStatic || sk == TokKind::KwExtern || sk == TokKind::KwAuto ||
				 sk == TokKind::KwRegister)
				++storageCount;
		};
		while(isQualOrStorage(peek().kind)) {
			applyQualStorage(peek().kind);
			advance();
		}
		auto finishSpec = [&] {
			if(isConst)
				out.quals |= 1u;
			setStorage(isStatic, isExtern, isInline);
		};
		if(storageCount > 1) {
			fail(peek(), "more than one storage-class specifier");
			return false;
		}
		if(peek().kind == TokKind::KwTypeof) {
			B32 ok = parseTypeofSpec(out);
			finishSpec();
			return ok;
		}
		if(peek().kind == TokKind::KwEnum) {
			B32 ok = parseEnumSpec(out);
			finishSpec();
			return ok;
		}
		if(peek().kind == TokKind::KwStruct || peek().kind == TokKind::KwUnion) {
			B32 ok = parseStructSpec(out);
			finishSpec();
			return ok;
		}
		if(peek().kind == TokKind::Identifier) {
			auto it = typedefs.find(lex.text(peek()));
			if(it != typedefs.end()) {
				advance();
				out = it->second;
				finishSpec();
				return true;
			}
		}

		B32 isVoid = false, isBool = false, isChar = false, isShort = false;
		B32 isUnsigned = false, isSigned = false;
		B32 isFloat = false, isDouble = false;
		B32 isComplex = false;
		I32 longCount = 0;
		I32 count = 0;
		for(;;) {
			TokKind k = peek().kind;
			if(k == TokKind::KwVoid)
				isVoid = true;
			else if(k == TokKind::KwBool)
				isBool = true;
			else if(k == TokKind::KwChar)
				isChar = true;
			else if(k == TokKind::KwShort)
				isShort = true;
			else if(k == TokKind::KwFloat)
				isFloat = true;
			else if(k == TokKind::KwDouble)
				isDouble = true;
			else if(k == TokKind::KwComplex || k == TokKind::KwImaginary)
				isComplex = true;
			else if(k == TokKind::KwLong)
				++longCount;
			else if(k == TokKind::KwUnsigned)
				isUnsigned = true;
			else if(k == TokKind::KwSigned)
				isSigned = true;
			else if(k == TokKind::KwInt)
				; // base int
			else if(isQualOrStorage(k)) {
				applyQualStorage(k);
				advance();
				continue;
			} else
				break;
			advance();
			++count;
		}
		if(count == 0)
			return false;
		if(storageCount > 1) {
			fail(peek(), "more than one storage-class specifier");
			return false;
		}
		CType t;
		if(isVoid) {
			t.isVoid = true;
		} else if(isFloat || isDouble || isComplex) {
			t.isFloat = true;
			t.bits = isFloat ? 32 : (isDouble && longCount >= 1 ? 128 : 64);
			t.isComplex = isComplex;
			if(isComplex)
				t.strukt = complexStruct(t);
		} else if(isBool) {
			t.bits = 1;
			t.isUnsigned = true;
		} else {
			t.isUnsigned = isUnsigned;
			if(isChar) {
				t.bits = 8;
				t.isPlainChar = !isUnsigned && !isSigned;
			} else if(isShort)
				t.bits = 16;
			else if(longCount >= 2) {
				t.bits = 64;
				t.isLongLong = true;
			} else if(longCount == 1)
				t.bits = 64;
			else
				t.bits = 32;
		}
		if(isConst)
			t.quals |= 1u;
		out = t;
		setStorage(isStatic, isExtern, isInline);
		return true;
	}

	void Parser::skipArrayQualifiers() {
		while(peek().kind == TokKind::KwStatic || isTypeQualifier(peek().kind))
			advance();
	}

	CType Parser::wrapArrayDims(CType base, const List<Dim>& dims) {
		for(U32 i = (U32)dims.size(); i-- > 0;) {
			ArrayType* at = arena.make<ArrayType>();
			at->elem = base;
			at->count = dims[i].count;
			at->countExpr = dims[i].expr;
			CType arr;
			arr.array = at;
			base = arr;
		}
		return base;
	}

	B32 Parser::parseArraySuffix(Declarator& d) {
		if(!check(TokKind::LBracket))
			return true;
		advance(); // [
		d.isArray = true;
		skipArrayQualifiers();
		if(peek().kind == TokKind::Star && peek2().kind == TokKind::RBracket) {
			advance(); // [*]
		} else if(peek().kind != TokKind::RBracket) {
			d.arrayLen = parseConditional(); // outermost bound
			if(!d.arrayLen)
				return false;
		}
		if(!expect(TokKind::RBracket, "']'"))
			return false;
		List<Dim> inner;
		while(accept(TokKind::LBracket)) {
			Expr* sz = parseConditional();
			if(!sz)
				return false;
			if(!expect(TokKind::RBracket, "']'"))
				return false;
			I64 n = 0;
			if(tryEvalIntConst(sz, n)) {
				if(n <= 0) {
					fail(peek(), "array dimension must be a positive integer constant");
					return false;
				}
				inner.push_back(Dim{(U32)n, nullptr});
			} else {
				inner.push_back(Dim{0, sz});
			}
		}
		d.type = wrapArrayDims(d.type, inner);
		return true;
	}

	B32 Parser::parseParamArray(CType& t) {
		Declarator d;
		d.type = t;
		if(!parseArraySuffix(d))
			return false;
		CType decayed = d.type;
		++decayed.ptr;
		t = decayed;
		return true;
	}

	B32 Parser::looksLikeFuncPtr() {
		return peek().kind == TokKind::LParen && peek2().kind == TokKind::Star;
	}

	void Parser::adjustParamType(CType& t, const Expr** vlaBound) {
		if(t.func != nullptr && t.ptr == 0) {
			t.ptr = 1;
		} else if(t.array != nullptr && t.ptr == 0) {
			if(vlaBound && t.array->countExpr)
				*vlaBound = t.array->countExpr;
			t = decay(t);
		}
	}

	B32 Parser::parseParamTypeList(FuncType* ft) {
		if(peek().kind == TokKind::RParen) {
			advance();
			ft->unprototyped = true;
			return true;
		}
		if(peek().kind == TokKind::KwVoid && peek2().kind == TokKind::RParen) {
			advance(); // void
			advance(); // )
			return true;
		}
		for(;;) {
			if(peek().kind == TokKind::Ellipsis) {
				advance();
				ft->isVarArgs = true;
				break;
			}
			CType pt;
			if(!parseTypeSpec(pt)) {
				fail(peek(), "expected parameter type");
				return false;
			}
			parsePointers(pt);
			const String* pname = nullptr;
			Token nameTok;
			B32 haveName = false;
			CType fpt;
			if(!parseDeclaratorType(pt, nameTok, haveName, fpt))
				return false;
			if(haveName)
				pname = arena.make<String>(lex.text(nameTok));
			pt = fpt;
			adjustParamType(pt);
			ft->params.push_back(pt);
			ft->paramNames.push_back(pname);
			if(!accept(TokKind::Comma))
				break;
		}
		return expect(TokKind::RParen, "')'");
	}

	B32 Parser::parseFuncPtrDeclarator(CType ret, Token& nameOut, CType& outType) {
		B32 haveName = false;
		return parseDeclaratorType(ret, nameOut, haveName, outType);
	}

	void Parser::bindDeclaratorType(Declarator& d, CType t, U32 offset) {
		if(isArrayType(t)) {
			d.isArray = true;
			U32 count = t.array->count;
			d.type = t.array->elem;
			if(count > 0) {
				Expr* len = makeExpr(ExprKind::IntLit, offset);
				len->intLit = {(I64)count, false, false};
				d.arrayLen = len;
			}
		} else {
			d.type = t;
		}
	}

	B32 Parser::looksLikeGroupingParen() {
		if(peek().kind != TokKind::LParen)
			return false;
		const Token& n = peek2();
		if(n.kind == TokKind::Star || n.kind == TokKind::LParen || n.kind == TokKind::LBracket)
			return true;
		if(n.kind == TokKind::Identifier)
			return !startsType(n);
		return false;
	}

	Parser::TypeBuilder Parser::parseDeclaratorSuffixes() {
		List<TypeBuilder> sfx;
		for(;;) {
			if(peek().kind == TokKind::LBracket) {
				advance(); // [
				skipArrayQualifiers();
				U32 count = 0;
				Expr* countExpr = nullptr;
				if(peek().kind == TokKind::Star && peek2().kind == TokKind::RBracket) {
					advance(); // [*]
				} else if(peek().kind != TokKind::RBracket) {
					Expr* e = parseConditional();
					if(!e)
						return {};
					I64 n = 0;
					if(tryEvalIntConst(e, n) && n > 0)
						count = (U32)n;
					else
						countExpr = e; // VLA
				}
				if(!expect(TokKind::RBracket, "']'"))
					return {};
				sfx.push_back([this, count, countExpr](CType b) {
					ArrayType* at = arena.make<ArrayType>();
					at->elem = b;
					at->count = count;
					at->countExpr = countExpr;
					CType a;
					a.array = at;
					return a;
				});
			} else if(peek().kind == TokKind::LParen) {
				advance(); // '('
				FuncType* ft = arena.make<FuncType>();
				if(!parseParamTypeList(ft))
					return {};
				sfx.push_back([ft](CType b) {
					ft->ret = b;
					CType t;
					t.func = ft;
					return t;
				});
			} else {
				break;
			}
		}
		return [sfx](CType base) {
			CType b = base;
			for(U32 i = (U32)sfx.size(); i-- > 0;)
				b = sfx[i](b);
			return b;
		};
	}

	Parser::TypeBuilder Parser::parseDirectDeclarator(Token& nameOut, B32& haveName) {
		TypeBuilder core;
		if(looksLikeGroupingParen()) {
			advance(); // (
			core = parseDeclaratorBuilder(nameOut, haveName);
			if(!expect(TokKind::RParen, "')'"))
				return {};
		} else if(peek().kind == TokKind::Identifier) {
			nameOut = advance();
			haveName = true;
			core = [](CType b) { return b; };
		} else {
			// abstract decl
			core = [](CType b) { return b; };
		}
		TypeBuilder suf = parseDeclaratorSuffixes();
		if(failed)
			return {};
		return [core, suf](CType base) { return core(suf(base)); };
	}

	Parser::TypeBuilder Parser::parseDeclaratorBuilder(Token& nameOut, B32& haveName) {
		U32 stars = 0;
		for(;;) {
			while(isTypeQualifier(peek().kind))
				advance();
			if(!accept(TokKind::Star))
				break;
			++stars;
		}
		TypeBuilder inner = parseDirectDeclarator(nameOut, haveName);
		if(failed)
			return {};
		return [stars, inner](CType base) {
			CType b = base;
			for(U32 i = 0; i < stars; ++i)
				b = pointerTo(b);
			return inner(b);
		};
	}

	B32 Parser::parseDeclaratorType(CType base, Token& nameOut, B32& haveName, CType& out) {
		haveName = false;
		TypeBuilder b = parseDeclaratorBuilder(nameOut, haveName);
		if(failed)
			return false;
		out = b(base);
		return true;
	}

	B32 Parser::parseStaticAssert() {
		Token kw = advance(); // _Static_assert
		if(!expect(TokKind::LParen, "'('"))
			return false;
		Expr* cond = parseConditional();
		if(!cond)
			return false;
		if(!expect(TokKind::Comma, "','"))
			return false;
		String msg;
		if(peek().kind != TokKind::StringLiteral) {
			fail(peek(), "expected a string literal in _Static_assert");
			return false;
		}
		if(!parseStringLiteral(advance(), msg))
			return false;
		while(peek().kind == TokKind::StringLiteral) {
			String more;
			if(!parseStringLiteral(advance(), more))
				return false;
			msg += more;
		}
		if(!expect(TokKind::RParen, "')'"))
			return false;
		if(!expect(TokKind::Semicolon, "';'"))
			return false;
		I64 v = 0;
		if(!evalIntConst(cond, v))
			return false;
		if(v == 0) {
			fail(kw, "static assertion failed: " + msg);
			return false;
		}
		return true;
	}

	B32 Parser::checkObjectComplete(const Declarator& d) {
		if(d.isExtern)
			return true;
		CType ult = d.type;
		while(ult.array != nullptr && ult.ptr == 0)
			ult = ult.array->elem;
		if(isStruct(ult) && (ult.strukt == nullptr || !ult.strukt->complete)) {
			fail(peek(), "variable has incomplete type");
			return false;
		}
		return true;
	}

	Stmt* Parser::parseDeclaration() {
		Token start = peek();
		if(peek().kind == TokKind::KwStaticAssert || peek().kind == TokKind::KwTypedef) {
			B32 ok = peek().kind == TokKind::KwStaticAssert ? parseStaticAssert() : parseTypedef();
			if(!ok)
				return nullptr;
			return makeStmt(StmtKind::Empty, start.offset);
		}
		CType base;
		if(!parseTypeSpec(base)) {
			fail(peek(), "expected type specifier");
			return nullptr;
		}
		B32 isStatic = sawStatic;
		B32 isExtern = sawExtern;
		Stmt* s = makeStmt(StmtKind::Decl, start.offset);
		if(peek().kind == TokKind::Semicolon) {
			advance();
			return s;
		}
		for(;;) {
			CType t = base;
			parsePointers(t);
			Declarator d;
			d.isStatic = isStatic;
			d.isExtern = isExtern;
			if(looksLikeGroupingParen()) {
				Token nameTok;
				B32 haveName = false;
				CType gt;
				if(!parseDeclaratorType(t, nameTok, haveName, gt))
					return nullptr;
				if(!haveName) {
					fail(peek(), "expected declarator name");
					return nullptr;
				}
				d.name = arena.make<String>(lex.text(nameTok));
				bindDeclaratorType(d, gt, nameTok.offset);
				d.offset = nameTok.offset;
			} else {
				if(peek().kind != TokKind::Identifier) {
					fail(peek(), "expected declarator name");
					return nullptr;
				}
				Token nameTok = advance();
				if(peek().kind == TokKind::LParen) {
					B32 more = false;
					FuncDef* proto = parseFunctionRest(t, nameTok, start, &more);
					if(!proto)
						return nullptr;
					blockProtos.push_back(proto);
					if(more)
						continue;
					return s;
				}
				d.name = arena.make<String>(lex.text(nameTok));
				d.type = t;
				d.offset = nameTok.offset;
				if(!parseArraySuffix(d))
					return nullptr;
				if(!d.isArray)
					bindDeclaratorType(d, t, nameTok.offset);
			}
			if(accept(TokKind::Assign)) {
				if(isExtern) {
					fail(start, "'extern' variable cannot have an initializer");
					return nullptr;
				}
				d.init = parseInitializer();
				if(!d.init)
					return nullptr;
			}
			if(!checkObjectComplete(d))
				return nullptr;
			s->decls.push_back(d);
			if(!accept(TokKind::Comma))
				break;
		}
		if(!expect(TokKind::Semicolon, "';'"))
			return nullptr;
		return s;
	}

	Expr* Parser::parseParenCond() {
		if(!expect(TokKind::LParen, "'('"))
			return nullptr;
		Expr* cond = parseExpression();
		if(!cond)
			return nullptr;
		if(!expect(TokKind::RParen, "')'"))
			return nullptr;
		return cond;
	}

	Stmt* Parser::parseIf() {
		Token kw = advance(); // if
		Expr* cond = parseParenCond();
		if(!cond)
			return nullptr;
		Stmt* thenS = parseStatement();
		if(!thenS)
			return nullptr;
		Stmt* elseS = nullptr;
		if(accept(TokKind::KwElse)) {
			elseS = parseStatement();
			if(!elseS)
				return nullptr;
		}
		Stmt* s = makeStmt(StmtKind::If, kw.offset);
		s->expr = cond;
		s->thenBody = thenS;
		s->elseBody = elseS;
		return s;
	}

	Stmt* Parser::parseWhile() {
		Token kw = advance(); // while
		Expr* cond = parseParenCond();
		if(!cond)
			return nullptr;
		Stmt* body = parseStatement();
		if(!body)
			return nullptr;
		Stmt* s = makeStmt(StmtKind::While, kw.offset);
		s->expr = cond;
		s->thenBody = body;
		return s;
	}

	Stmt* Parser::parseDoWhile() {
		Token kw = advance(); // do
		Stmt* body = parseStatement();
		if(!body)
			return nullptr;
		if(!expect(TokKind::KwWhile, "'while'"))
			return nullptr;
		Expr* cond = parseParenCond();
		if(!cond)
			return nullptr;
		if(!expect(TokKind::Semicolon, "';'"))
			return nullptr;
		Stmt* s = makeStmt(StmtKind::DoWhile, kw.offset);
		s->expr = cond;
		s->thenBody = body;
		return s;
	}

	Stmt* Parser::parseFor() {
		Token kw = advance(); // for
		if(!expect(TokKind::LParen, "'('"))
			return nullptr;

		// init clause
		Stmt* init = nullptr;
		if(startsType(peek()) || peek().kind == TokKind::KwTypedef) {
			init = parseDeclaration();
			if(!init)
				return nullptr;
		} else if(peek().kind != TokKind::Semicolon) {
			Token at = peek();
			Expr* e = parseExpression();
			if(!e)
				return nullptr;
			if(!expect(TokKind::Semicolon, "';'"))
				return nullptr;
			init = makeStmt(StmtKind::Expr, at.offset);
			init->expr = e;
		} else {
			advance(); // empty init
		}

		// condition
		Expr* cond = nullptr;
		if(peek().kind != TokKind::Semicolon) {
			cond = parseExpression();
			if(!cond)
				return nullptr;
		}
		if(!expect(TokKind::Semicolon, "';'"))
			return nullptr;

		// post expression
		Expr* post = nullptr;
		if(peek().kind != TokKind::RParen) {
			post = parseExpression();
			if(!post)
				return nullptr;
		}
		if(!expect(TokKind::RParen, "')'"))
			return nullptr;

		Stmt* body = parseStatement();
		if(!body)
			return nullptr;

		Stmt* s = makeStmt(StmtKind::For, kw.offset);
		s->forInit = init;
		s->expr = cond;
		s->forPost = post;
		s->thenBody = body;
		return s;
	}

	Stmt* Parser::parseSwitch() {
		Token kw = advance(); // switch
		Expr* ctrl = parseParenCond();
		if(!ctrl)
			return nullptr;
		Stmt* body;
		if(peek().kind == TokKind::LBrace) {
			body = parseStatement();
			if(!body)
				return nullptr;
		} else {
			Stmt* block = makeStmt(StmtKind::Compound, peek().offset);
			while(peek().kind == TokKind::KwCase || peek().kind == TokKind::KwDefault) {
				Stmt* marker = parseStatement();
				if(!marker)
					return nullptr;
				block->body.push_back(marker);
			}
			Stmt* inner = parseStatement();
			if(!inner)
				return nullptr;
			block->body.push_back(inner);
			body = block;
		}
		Stmt* s = makeStmt(StmtKind::Switch, kw.offset);
		s->expr = ctrl;
		s->thenBody = body;
		return s;
	}

	Stmt* Parser::parseStatement() {
		const Token& tok = peek();
		if(tok.kind == TokKind::LBrace)
			return parseCompound();

		if(tok.kind == TokKind::Identifier && peek2().kind == TokKind::Colon) {
			Token nameTok = advance();
			advance(); // ':'
			Stmt* sub = parseStatement();
			if(!sub)
				return nullptr;
			Stmt* s = makeStmt(StmtKind::Label, nameTok.offset);
			s->label = arena.make<String>(lex.text(nameTok));
			s->thenBody = sub;
			return s;
		}

		if(tok.kind == TokKind::KwGoto) {
			Token kw = advance();
			if(!check(TokKind::Identifier)) {
				fail(peek(), "expected a label name after 'goto'");
				return nullptr;
			}
			Token nameTok = advance();
			Stmt* s = makeStmt(StmtKind::Goto, kw.offset);
			s->label = arena.make<String>(lex.text(nameTok));
			if(!expect(TokKind::Semicolon, "';'"))
				return nullptr;
			return s;
		}

		if(startsType(tok) || tok.kind == TokKind::KwTypedef || tok.kind == TokKind::KwStaticAssert)
			return parseDeclaration();

		if(tok.kind == TokKind::KwIf)
			return parseIf();
		if(tok.kind == TokKind::KwWhile)
			return parseWhile();
		if(tok.kind == TokKind::KwDo)
			return parseDoWhile();
		if(tok.kind == TokKind::KwFor)
			return parseFor();
		if(tok.kind == TokKind::KwSwitch)
			return parseSwitch();

		if(tok.kind == TokKind::KwCase) {
			Token kw = advance();
			Expr* value = parseConditional();
			if(!value)
				return nullptr;
			if(!expect(TokKind::Colon, "':'"))
				return nullptr;
			Stmt* s = makeStmt(StmtKind::Case, kw.offset);
			s->expr = value;
			return s;
		}

		if(tok.kind == TokKind::KwDefault) {
			Token kw = advance();
			if(!expect(TokKind::Colon, "':'"))
				return nullptr;
			return makeStmt(StmtKind::Default, kw.offset);
		}

		if(tok.kind == TokKind::KwBreak || tok.kind == TokKind::KwContinue) {
			Token kw = advance();
			Stmt* s =
					makeStmt(kw.kind == TokKind::KwBreak ? StmtKind::Break : StmtKind::Continue, kw.offset);
			if(!expect(TokKind::Semicolon, "';'"))
				return nullptr;
			return s;
		}

		if(tok.kind == TokKind::KwReturn) {
			Token kw = advance();
			Stmt* s = makeStmt(StmtKind::Return, kw.offset);
			if(peek().kind != TokKind::Semicolon) {
				s->expr = parseExpression();
				if(!s->expr)
					return nullptr;
			}
			if(!expect(TokKind::Semicolon, "';'"))
				return nullptr;
			return s;
		}

		if(tok.kind == TokKind::Semicolon) {
			Token semi = advance();
			return makeStmt(StmtKind::Empty, semi.offset);
		}

		Stmt* s = makeStmt(StmtKind::Expr, tok.offset);
		s->expr = parseExpression();
		if(!s->expr)
			return nullptr;
		if(!expect(TokKind::Semicolon, "';'"))
			return nullptr;
		return s;
	}

	Stmt* Parser::parseCompound() {
		Token open = peek();
		if(!expect(TokKind::LBrace, "'{'"))
			return nullptr;
		Stmt* block = makeStmt(StmtKind::Compound, open.offset);
		Map<String, CType> savedTypedefs = typedefs;
		Map<String, I64> savedEnumConstants = enumConstants;
		Map<String, StructType*> savedStructTypes = structTypes;
		while(!failed && peek().kind != TokKind::RBrace && peek().kind != TokKind::Eof) {
			Stmt* s = parseStatement();
			if(!s)
				return nullptr;
			block->body.push_back(s);
		}
		typedefs = std::move(savedTypedefs);
		enumConstants = std::move(savedEnumConstants);
		structTypes = std::move(savedStructTypes);
		if(!expect(TokKind::RBrace, "'}'"))
			return nullptr;
		return block;
	}

	B32 Parser::checkParamNames(const FuncDef* fn) {
		for(U32 i = 0; i < fn->params.size(); i++) {
			if(!fn->params[i].name)
				continue;
			for(U32 j = 0; j < i; j++) {
				if(fn->params[j].name && *fn->params[j].name == *fn->params[i].name) {
					fail(peek(), "redefinition of parameter");
					return false;
				}
			}
		}
		return true;
	}

	FuncDef* Parser::parseFunctionRest(CType ret,
																		 const Token& nameTok,
																		 const Token& start,
																		 B32* moreDeclarators) {
		if(!expect(TokKind::LParen, "'('"))
			return nullptr;

		FuncDef* fn = arena.make<FuncDef>();
		fn->name = lex.text(nameTok);
		fn->retType = ret;
		fn->offset = start.offset;

		if(peek().kind == TokKind::RParen) {
			fn->unprototyped = true;
		} else if(peek().kind == TokKind::KwVoid && peek2().kind == TokKind::RParen) {
			advance(); // (void)
		} else if(peek().kind == TokKind::Identifier && !startsType(peek())) {
			if(!parseOldStyleParams(fn))
				return nullptr;
			if(!checkParamNames(fn))
				return nullptr;
			curFuncName = fn->name;
			fn->body = parseCompound();
			curFuncName.clear();
			return fn->body ? fn : nullptr;
		} else if(peek().kind != TokKind::RParen) {
			for(;;) {
				if(peek().kind == TokKind::Ellipsis) {
					if(fn->params.empty()) {
						fail(peek(), "'...' must be preceded by a named parameter");
						return nullptr;
					}
					advance();
					fn->isVarArgs = true;
					break;
				}
				Token pstart = peek();
				CType pt;
				if(!parseTypeSpec(pt)) {
					fail(peek(), "expected parameter type");
					return nullptr;
				}
				parsePointers(pt);
				Param p;
				p.offset = pstart.offset;
				Token pnameTok;
				B32 haveName = false;
				CType fpt;
				if(!parseDeclaratorType(pt, pnameTok, haveName, fpt))
					return nullptr;
				pt = fpt;
				if(isVoidType(pt)) {
					fail(pstart, "'void' must be the only unnamed parameter");
					return nullptr;
				}
				adjustParamType(pt, &p.vlaBound);
				p.type = pt;
				if(haveName)
					p.name = arena.make<String>(lex.text(pnameTok));
				fn->params.push_back(p);
				if(!accept(TokKind::Comma))
					break;
			}
		}
		if(!expect(TokKind::RParen, "')'"))
			return nullptr;

		if(accept(TokKind::Semicolon))
			return fn;

		if(moreDeclarators && peek().kind == TokKind::Comma) {
			advance();
			*moreDeclarators = true;
			return fn;
		}

		if(!checkParamNames(fn))
			return nullptr;
		curFuncName = fn->name;
		fn->body = parseCompound();
		curFuncName.clear();
		if(!fn->body)
			return nullptr;
		return fn;
	}

	B32 Parser::parseOldStyleParams(FuncDef* fn) {
		for(;;) {
			Token nameTok = peek();
			if(nameTok.kind != TokKind::Identifier) {
				fail(nameTok, "expected parameter name");
				return false;
			}
			advance();
			Param p;
			p.name = arena.make<String>(lex.text(nameTok));
			p.type = ctInt();
			p.offset = nameTok.offset;
			fn->params.push_back(p);
			if(!accept(TokKind::Comma))
				break;
		}
		if(!expect(TokKind::RParen, "')'"))
			return false;
		while(startsType(peek())) {
			CType base;
			if(!parseTypeSpec(base)) {
				fail(peek(), "expected parameter type");
				return false;
			}
			for(;;) {
				CType t = base;
				Token pnameTok;
				B32 haveName = false;
				CType decl;
				if(!parseDeclaratorType(t, pnameTok, haveName, decl))
					return false;
				if(!haveName) {
					fail(peek(), "expected parameter name");
					return false;
				}
				adjustParamType(decl);
				String name = lex.text(pnameTok);
				B32 matched = false;
				for(U32 i = 0; i < fn->params.size(); i++) {
					if(fn->params[i].name && *fn->params[i].name == name) {
						fn->params[i].type = decl;
						matched = true;
						break;
					}
				}
				if(!matched) {
					fail(pnameTok,
							 "parameter named in declaration is not in the "
							 "identifier list");
					return false;
				}
				if(!accept(TokKind::Comma))
					break;
			}
			if(!expect(TokKind::Semicolon, "';'"))
				return false;
		}
		return true;
	}

	Stmt* Parser::parseGlobalRest(CType base, Declarator first, const Token& start) {
		Stmt* s = makeStmt(StmtKind::Decl, start.offset);
		Declarator d = first;
		for(;;) {
			CType raw = d.type;
			if(!parseArraySuffix(d))
				return nullptr;
			if(!d.isArray)
				bindDeclaratorType(d, raw, d.offset);
			if(accept(TokKind::Assign)) {
				d.init = parseInitializer();
				if(!d.init)
					return nullptr;
			}
			if(!checkObjectComplete(d))
				return nullptr;
			s->decls.push_back(d);
			if(!accept(TokKind::Comma))
				break;
			CType t = base;
			parsePointers(t);
			Declarator prev = d;
			d = Declarator{};
			d.isExtern = prev.isExtern;
			d.isStatic = prev.isStatic;
			if(looksLikeGroupingParen()) {
				Token nameTok;
				B32 haveName = false;
				CType gt;
				if(!parseDeclaratorType(t, nameTok, haveName, gt))
					return nullptr;
				if(!haveName) {
					fail(peek(), "expected declarator name");
					return nullptr;
				}
				d.name = arena.make<String>(lex.text(nameTok));
				bindDeclaratorType(d, gt, nameTok.offset);
				d.offset = nameTok.offset;
			} else {
				if(peek().kind != TokKind::Identifier) {
					fail(peek(), "expected declarator name");
					return nullptr;
				}
				Token nameTok = advance();
				d.name = arena.make<String>(lex.text(nameTok));
				d.type = t;
				d.offset = nameTok.offset;
			}
		}
		if(!expect(TokKind::Semicolon, "';'"))
			return nullptr;
		return s;
	}

	B32 Parser::parseSharedDeclarators(CType base, TransUnit* unit, const Token& start) {
		for(;;) {
			CType t = base;
			parsePointers(t);
			if(peek().kind != TokKind::Identifier) {
				fail(peek(), "expected declarator name");
				return false;
			}
			Token nameTok = advance();
			if(peek().kind == TokKind::LParen) {
				B32 more = false;
				FuncDef* fn = parseFunctionRest(t, nameTok, start, &more);
				if(!fn)
					return false;
				unit->functions.push_back(fn);
				if(more)
					continue;
				return true;
			}
			Declarator d;
			d.name = arena.make<String>(lex.text(nameTok));
			d.type = t;
			d.offset = nameTok.offset;
			Stmt* g = parseGlobalRest(base, d, start);
			if(!g)
				return false;
			unit->globals.push_back(g);
			return true;
		}
	}

	B32 Parser::registerFuncDef(FuncDef* fn) {
		if(!fn->body)
			return true;
		auto it = funcDefs.find(fn->name);
		if(it != funcDefs.end()) {
			fail(peek(), "redefinition of function");
			return false;
		}
		funcDefs.emplace(fn->name, fn);
		return true;
	}

	TransUnit* Parser::parseUnit() {
		TransUnit* unit = arena.make<TransUnit>();
		while(!failed && peek().kind != TokKind::Eof) {
			Token start = peek();
			if(peek().kind == TokKind::KwStaticAssert) {
				if(!parseStaticAssert())
					return nullptr;
				continue;
			}
			if(peek().kind == TokKind::KwTypedef) {
				if(!parseTypedef())
					return nullptr;
				continue;
			}
			CType base;
			if(!parseTypeSpec(base)) {
				fail(peek(), "expected type specifier");
				return nullptr;
			}
			B32 gExtern = sawExtern;
			B32 gExternInline = sawExtern && sawInline;
			CType first = base;
			parsePointers(first);
			if(first.ptr == 0 && peek().kind == TokKind::Semicolon) {
				advance();
				continue;
			}

			if(looksLikeFuncPtr()) {
				Token nameTok;
				CType fpt;
				if(!parseFuncPtrDeclarator(first, nameTok, fpt))
					return nullptr;

				if(fpt.func && fpt.ptr == 0) {
					FuncDef* fn = arena.make<FuncDef>();
					fn->name = lex.text(nameTok);
					fn->retType = fpt.func->ret;
					fn->isVarArgs = fpt.func->isVarArgs;
					fn->offset = start.offset;
					for(U32 i = 0; i < fpt.func->params.size(); ++i) {
						Param p;
						p.type = fpt.func->params[i];
						if(i < fpt.func->paramNames.size())
							p.name = fpt.func->paramNames[i];
						fn->params.push_back(p);
					}
					if(accept(TokKind::Semicolon)) {
						unit->functions.push_back(fn); // prototype
						continue;
					}
					curFuncName = fn->name;
					fn->body = parseCompound();
					curFuncName.clear();
					if(!fn->body)
						return nullptr;
					if(!registerFuncDef(fn))
						return nullptr;
					unit->functions.push_back(fn);
					continue;
				}
				Declarator d;
				d.isExtern = gExtern;
				d.name = arena.make<String>(lex.text(nameTok));
				bindDeclaratorType(d, fpt, nameTok.offset);
				d.offset = nameTok.offset;
				Stmt* g = parseGlobalRest(base, d, start);
				if(!g)
					return nullptr;
				unit->globals.push_back(g);
				continue;
			}
			if(peek().kind != TokKind::Identifier) {
				fail(peek(), "expected name");
				return nullptr;
			}
			Token nameTok = advance();
			if(peek().kind == TokKind::LParen) {
				B32 more = false;
				FuncDef* fn = parseFunctionRest(first, nameTok, start, &more);
				if(!fn)
					return nullptr;
				fn->isExternInline = gExternInline;
				if(!registerFuncDef(fn))
					return nullptr;
				unit->functions.push_back(fn);
				if(more) {
					if(!parseSharedDeclarators(base, unit, start))
						return nullptr;
				}
			} else {
				Declarator d;
				d.isExtern = gExtern;
				d.name = arena.make<String>(lex.text(nameTok));
				d.type = first;
				d.offset = nameTok.offset;
				Stmt* g = parseGlobalRest(base, d, start);
				if(!g)
					return nullptr;
				unit->globals.push_back(g);
			}
		}
		if(failed)
			return nullptr;
		for(FuncDef* proto : blockProtos)
			unit->functions.push_back(proto);
		return unit;
	}
} // namespace rat::cc
