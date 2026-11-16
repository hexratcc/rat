#include "parse/parser.h"

#include "parse/parser_detail.h"

namespace rat::cc {
	// [ [qualifier]... * ]... [qualifier]...
	// const, volatile and restrict are kept, one bit per pointer level
	void Parser::parsePointers(CType& t) {
		for(;;) {
			while(detail::isTypeQualifier(peek().kind)) {
				TokKind qual = advance().kind;
				if(qual == TokKind::KwConst && t.ptr < 32)
					setTopConst(t);
				if(qual == TokKind::KwVolatile && t.ptr < 32)
					setTopVolatile(t);
				if(qual == TokKind::KwRestrict && t.ptr < 32)
					setTopRestrict(t);
			}
			if(!accept(TokKind::Star))
				break;
			++t.ptr;
		}
	}

	// [cond-expr] ']'
	// a bound that is not a positive constant is kept as a VLA expr
	B32 Parser::parseArrayBound(DeclOp& op) {
		if(!check(TokKind::RBracket)) {
			Expr* e = parseConditional();
			if(!e)
				return false;
			op.bound = e;
			I64 n = 0;
			if(tryEvalIntConst(e, n) && n > 0)
				op.count = (U64)n;
		}
		return expect(TokKind::RBracket);
	}

	// a function parameter decays to a pointer, an array to a pointer to its element
	void Parser::adjustParamType(CType& t, const Expr** vlaBound) {
		if(t.func != nullptr && t.ptr == 0) {
			t.ptr = 1;
		} else if(t.array != nullptr && t.ptr == 0) {
			if(vlaBound && isVlaType(t))
				*vlaBound = t.array->countExpr;
			t = decay(t);
		}
	}

	// name [, name]... )
	B32 Parser::parseParamNames(FuncType* ft) {
		for(;;) {
			if(!check(TokKind::Identifier))
				return fail(peek(), "expected parameter name");
			Token nameTok = advance();
			ft->params.push_back({arena.make<String>(lex.text(nameTok)), ctInt(), nameTok.offset});
			if(!accept(TokKind::Comma))
				break;
		}
		return expect(TokKind::RParen);
	}

	// [ void | param [, param]... [, '...'] ] ) | param-names
	// param: type-spec declarator
	// a bare name list is old style: the types follow in a declaration list before the body
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
		if(check(TokKind::Identifier) && !startsType(peek())) {
			ft->oldStyle = true;
			return parseParamNames(ft);
		}
		for(;;) {
			if(check(TokKind::Ellipsis)) {
				if(ft->params.empty())
					return fail(peek(), "'...' must be preceded by a named parameter");
				advance();
				ft->isVarArgs = true;
				break;
			}
			Token pstart = peek();
			CType pt;
			if(!parseTypeSpec(pt))
				return fail(peek(), "expected parameter type");
			DeclResult r;
			if(!parseDeclarator(pt, r))
				return false;
			if(isVoidType(r.type))
				return fail(pstart, "'void' must be the only unnamed parameter");
			Param p{r.name, r.type, pstart.offset};
			adjustParamType(p.type, &p.vlaBound);
			ft->params.push_back(p);
			if(!accept(TokKind::Comma))
				break;
		}
		return expect(TokKind::RParen);
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
		return n.kind == TokKind::Identifier && !startsType(n);
	}

	// folds the ops onto base, innermost first
	CType Parser::applyDeclOps(CType base, const List<DeclOp>& ops) {
		CType b = base;
		for(const DeclOp& op : ops) {
			switch(op.kind) {
			case DeclOp::Kind::Pointer:
				b.ptr += (U32)op.count;
				break;
			case DeclOp::Kind::Array: {
				ArrayType* at = arena.make<ArrayType>();
				at->elem = b;
				at->count = op.count;
				at->countExpr = op.bound;
				b = CType{};
				b.array = at;
				break;
			}
			case DeclOp::Kind::Func:
				op.func->ret = b;
				b = CType{};
				b.func = op.func;
				break;
			}
		}
		return b;
	}

	// [ '[' [ static | qualifier ]... [ * | cond-expr ] ']' | ( param-type-list | alignas ]...
	// pushed onto ops innermost first
	B32 Parser::parseDeclaratorSuffixes(List<DeclOp>& ops, U32& align) {
		List<DeclOp> sfx;
		for(;;) {
			if(accept(TokKind::LBracket)) {
				while(check(TokKind::KwStatic) || detail::isTypeQualifier(peek().kind))
					advance();
				DeclOp op{DeclOp::Kind::Array};
				if(check(TokKind::Star) && peek2().kind == TokKind::RBracket)
					advance(); // [*]
				if(!parseArrayBound(op))
					return false;
				sfx.push_back(op);
			} else if(accept(TokKind::LParen)) {
				DeclOp op{DeclOp::Kind::Func, 0, nullptr, arena.make<FuncType>()};
				if(!parseParamTypeList(op.func))
					return false;
				sfx.push_back(op);
			} else if(check(TokKind::KwAlignas)) {
				if(!acceptTrailingAlignas(align))
					return false;
			} else {
				break;
			}
		}
		ops.insert(ops.end(), sfx.rbegin(), sfx.rend());
		return true;
	}

	// [ ( declarator ) | name ] suffixes
	B32 Parser::parseDirectDeclarator(List<DeclOp>& ops, DeclResult& out) {
		List<DeclOp> inner;
		if(looksLikeGroupingParen()) {
			advance(); // (
			if(!parseDeclaratorOps(inner, out) || !expect(TokKind::RParen))
				return false;
		} else if(check(TokKind::Identifier)) {
			Token nameTok = advance();
			out.name = arena.make<String>(lex.text(nameTok));
			out.offset = nameTok.offset;
		}
		if(!parseDeclaratorSuffixes(ops, out.align) || failed)
			return false;
		ops.insert(ops.end(), inner.begin(), inner.end());
		return true;
	}

	// [ [qualifier]... * ]... [qualifier]... direct-declarator
	B32 Parser::parseDeclaratorOps(List<DeclOp>& ops, DeclResult& out) {
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
		if(stars)
			ops.push_back({DeclOp::Kind::Pointer, stars});
		return parseDirectDeclarator(ops, out);
	}

	// pointers declarator-ops, resolved against base
	// an old-style name list is only allowed on the outermost function declarator of a definition
	B32 Parser::parseDeclarator(CType base, DeclResult& out) {
		CType t = base;
		parsePointers(t);
		List<DeclOp> ops;
		if(!parseDeclaratorOps(ops, out) || failed)
			return false;
		for(U32 i = 0; i < ops.size(); ++i) {
			B32 oldStyle = ops[i].kind == DeclOp::Kind::Func && ops[i].func->oldStyle;
			if(oldStyle && (i + 1 < ops.size() || !out.allowOldStyle))
				return fail(peek(), "parameter names (without types) in function declaration");
		}
		out.type = applyDeclOps(t, ops);
		return true;
	}

	void Parser::bindDeclarator(Declarator& d, const DeclResult& r) {
		d.name = r.name;
		d.offset = r.offset;
		d.align = r.align;
		d.type = r.type;
		if(isArrayType(d.type))
			d.type = plainArray(d.type);
	}

	// declarator without a name
	B32 Parser::parseAbstractDeclarator(CType base, DeclResult& out) {
		if(!parseDeclarator(base, out))
			return false;
		if(out.name)
			return fail(peek(), "unexpected name in a type name");
		return true;
	}
} // namespace rat::cc
