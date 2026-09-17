#include "parse/parser.h"

#include "parse/parser_detail.h"

namespace rat::cc {
	// [ static | qualifier ]... inside an array bound
	void Parser::skipArrayQualifiers() {
		while(check(TokKind::KwStatic) || detail::isTypeQualifier(peek().kind))
			advance();
	}

	// wraps base in one array level per dim, dims[0] outermost
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

	// [ '[' [ static | qualifier ]... [ * | cond-expr ] ']' [ '[' cond-expr ']' ]... ] [alignas]...
	// the outermost bound goes to d.arrayLen, the inner ones wrap d.type
	B32 Parser::parseArraySuffix(Declarator& d, U32* align) {
		U32 sink = 0;
		U32& objAlign = align ? *align : sink;
		if(!check(TokKind::LBracket))
			return acceptTrailingAlignas(objAlign);
		advance(); // [
		d.isArray = true;
		skipArrayQualifiers();
		if(check(TokKind::Star) && peek2().kind == TokKind::RBracket) {
			advance(); // [*]
		} else if(!check(TokKind::RBracket)) {
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
				inner.push_back(Dim{(U64)n, nullptr});
			} else {
				inner.push_back(Dim{0, sz});
			}
		}
		d.type = wrapArrayDims(d.type, inner);
		return acceptTrailingAlignas(objAlign);
	}

	// a function parameter decays to a pointer, an array to a pointer to its element
	void Parser::adjustParamType(CType& t, const Expr** vlaBound) {
		if(t.func != nullptr && t.ptr == 0) {
			t.ptr = 1;
		} else if(t.array != nullptr && t.ptr == 0) {
			if(vlaBound && t.array->countExpr)
				*vlaBound = t.array->countExpr;
			t = decay(t);
		}
	}

	// [ void | '...' | param [, param]... [, '...'] ] )
	// param: type-spec declarator
	B32 Parser::parseParamTypeList(FuncType* ft) {
		if(accept(TokKind::RParen)) {
			ft->unprototyped = true;
			return true;
		}
		if(check(TokKind::KwVoid) && peek2().kind == TokKind::RParen) {
			advance(); // void
			advance(); // )
			return true;
		}
		for(;;) {
			if(accept(TokKind::Ellipsis)) {
				ft->isVarArgs = true;
				break;
			}
			CType pt;
			if(!parseTypeSpec(pt)) {
				fail(peek(), "expected parameter type");
				return false;
			}
			parsePointers(pt);
			Token nameTok;
			B32 haveName = false;
			Param p;
			if(!parseDeclaratorType(pt, nameTok, haveName, p.type))
				return false;
			if(haveName)
				p.name = arena.make<String>(lex.text(nameTok));
			adjustParamType(p.type);
			ft->params.push_back(p);
			if(!accept(TokKind::Comma))
				break;
		}
		return expect(TokKind::RParen, "')'");
	}

	// splits an array type into the declarator's element type and length
	void Parser::bindDeclaratorType(Declarator& d, CType t, U32 offset) {
		if(isArrayType(t)) {
			d.isArray = true;
			U64 count = t.array->count;
			d.type = t.array->elem;
			if(count > 0) {
				Expr* len = makeExpr(ExprKind::IntLit, offset);
				len->intLit = {(I64)count, 32, 0};
				d.arrayLen = len;
			}
		} else {
			d.type = t;
		}
	}

	// ( followed by * ( '[' noinline or a non-type name: a parenthesized declarator
	B32 Parser::looksLikeGroupingParen() {
		if(!check(TokKind::LParen))
			return false;
		const Token& n = peek2();
		if(n.kind == TokKind::Star || n.kind == TokKind::LParen || n.kind == TokKind::LBracket)
			return true;
		if(n.kind == TokKind::KwNoinline)
			return true;
		if(n.kind == TokKind::Identifier)
			return !startsType(n);
		return false;
	}

	// folds the ops onto base, innermost first
	CType Parser::applyDeclOps(CType base, const List<DeclOp>& ops) {
		CType b = base;
		for(const DeclOp& op : ops) {
			switch(op.kind) {
			case DeclOp::Kind::Pointer:
				for(U32 i = 0; i < op.count; ++i)
					b = pointerTo(b);
				break;
			case DeclOp::Kind::Array: {
				ArrayType* at = arena.make<ArrayType>();
				at->elem = b;
				at->count = op.count;
				at->countExpr = op.countExpr;
				CType a;
				a.array = at;
				b = a;
				break;
			}
			case DeclOp::Kind::Func: {
				op.func->ret = b;
				CType t;
				t.func = op.func;
				b = t;
				break;
			}
			}
		}
		return b;
	}

	// [cond-expr] ']'
	// a bound that is not a positive constant is kept as a VLA expr
	B32 Parser::parseArrayBound(U64& count, Expr*& expr) {
		if(!check(TokKind::RBracket)) {
			Expr* e = parseConditional();
			if(!e)
				return false;
			I64 n = 0;
			if(tryEvalIntConst(e, n) && n > 0)
				count = (U64)n;
			else
				expr = e; // VLA
		}
		return expect(TokKind::RBracket, "']'");
	}

	// [ '[' [ static | qualifier ]... [ * | cond-expr ] ']' | ( param-type-list | alignas ]...
	// pushed onto ops innermost first
	B32 Parser::parseDeclaratorSuffixes(List<DeclOp>& ops) {
		List<DeclOp> sfx;
		for(;;) {
			if(accept(TokKind::LBracket)) {
				skipArrayQualifiers();
				DeclOp op;
				op.kind = DeclOp::Kind::Array;
				if(check(TokKind::Star) && peek2().kind == TokKind::RBracket)
					advance(); // [*]
				if(!parseArrayBound(op.count, op.countExpr))
					return false;
				sfx.push_back(op);
			} else if(accept(TokKind::LParen)) {
				DeclOp op;
				op.kind = DeclOp::Kind::Func;
				op.func = arena.make<FuncType>();
				if(!parseParamTypeList(op.func))
					return false;
				sfx.push_back(op);
			} else if(check(TokKind::KwAlignas)) {
				U32 ignored = 0;
				if(!acceptTrailingAlignas(ignored))
					return false;
			} else {
				break;
			}
		}
		for(U32 i = (U32)sfx.size(); i-- > 0;)
			ops.push_back(sfx[i]);
		return true;
	}

	// [ ( declarator ) | name ] suffixes
	B32 Parser::parseDirectDeclarator(List<DeclOp>& ops, Token& nameOut, B32& haveName) {
		B32 grouped = false;
		List<DeclOp> inner;
		if(looksLikeGroupingParen()) {
			advance(); // (
			grouped = true;
			if(!parseDeclaratorOps(inner, nameOut, haveName))
				return false;
			if(!expect(TokKind::RParen, "')'"))
				return false;
		} else if(check(TokKind::Identifier)) {
			nameOut = advance();
			haveName = true;
		}
		if(!parseDeclaratorSuffixes(ops))
			return false;
		if(failed)
			return false;
		if(grouped)
			for(const DeclOp& op : inner)
				ops.push_back(op);
		return true;
	}

	// [ [qualifier]... * ]... [qualifier]... direct-declarator
	B32 Parser::parseDeclaratorOps(List<DeclOp>& ops, Token& nameOut, B32& haveName) {
		DepthScope scope(*this);
		if(!enterDepth())
			return false;
		U32 stars = 0;
		for(;;) {
			while(detail::isTypeQualifier(peek().kind))
				advance();
			if(!accept(TokKind::Star))
				break;
			++stars;
		}
		if(stars) {
			DeclOp op;
			op.kind = DeclOp::Kind::Pointer;
			op.count = stars;
			ops.push_back(op);
		}
		return parseDirectDeclarator(ops, nameOut, haveName);
	}

	// declarator, resolved against base
	B32 Parser::parseDeclaratorType(CType base, Token& nameOut, B32& haveName, CType& out) {
		haveName = false;
		List<DeclOp> ops;
		if(!parseDeclaratorOps(ops, nameOut, haveName) || failed)
			return false;
		out = applyDeclOps(base, ops);
		return true;
	}

} // namespace rat::cc
