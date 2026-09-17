#include "parse/parser.h"

#include "parse/parser_detail.h"

namespace rat::cc {
	// [ [qualifier]... * ]... [qualifier]...
	// only const is kept, one bit per pointer level
	void Parser::parsePointers(CType& t) {
		for(;;) {
			B32 sawConst = false;
			while(detail::isTypeQualifier(peek().kind)) {
				if(check(TokKind::KwConst))
					sawConst = true;
				advance();
			}
			if(sawConst && t.ptr < 32)
				setTopConst(t);
			if(!accept(TokKind::Star))
				break;
			++t.ptr;
		}
	}

	// [ static | qualifier ]... inside an array bound
	void Parser::skipArrayQualifiers() {
		while(check(TokKind::KwStatic) || detail::isTypeQualifier(peek().kind))
			advance();
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
		return expect(TokKind::RBracket, "']'");
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

	// name [, name]... )
	B32 Parser::parseParamNames(FuncType* ft) {
		for(;;) {
			if(!check(TokKind::Identifier)) {
				fail(peek(), "expected parameter name");
				return false;
			}
			Token nameTok = advance();
			Param p;
			p.name = arena.make<String>(lex.text(nameTok));
			p.offset = nameTok.offset;
			ft->params.push_back(p);
			if(!accept(TokKind::Comma))
				break;
		}
		return expect(TokKind::RParen, "')'");
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
				if(ft->params.empty()) {
					fail(peek(), "'...' must be preceded by a named parameter");
					return false;
				}
				advance();
				ft->isVarArgs = true;
				break;
			}
			Token pstart = peek();
			CType pt;
			if(!parseTypeSpec(pt)) {
				fail(peek(), "expected parameter type");
				return false;
			}
			DeclResult r;
			if(!parseDeclarator(pt, r))
				return false;
			if(isVoidType(r.type)) {
				fail(pstart, "'void' must be the only unnamed parameter");
				return false;
			}
			Param p;
			p.name = r.name;
			p.offset = pstart.offset;
			p.type = r.type;
			adjustParamType(p.type, &p.vlaBound);
			ft->params.push_back(p);
			if(!accept(TokKind::Comma))
				break;
		}
		return expect(TokKind::RParen, "')'");
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
				if(op.count == 0)
					at->countExpr = op.bound;
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

	// [ '[' [ static | qualifier ]... [ * | cond-expr ] ']' | ( param-type-list | alignas ]...
	// pushed onto ops innermost first
	B32 Parser::parseDeclaratorSuffixes(List<DeclOp>& ops, U32& align) {
		List<DeclOp> sfx;
		for(;;) {
			if(accept(TokKind::LBracket)) {
				skipArrayQualifiers();
				DeclOp op;
				op.kind = DeclOp::Kind::Array;
				if(check(TokKind::Star) && peek2().kind == TokKind::RBracket)
					advance(); // [*]
				if(!parseArrayBound(op))
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
				if(!acceptTrailingAlignas(align))
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
	B32 Parser::parseDirectDeclarator(List<DeclOp>& ops, DeclResult& out) {
		B32 grouped = false;
		List<DeclOp> inner;
		if(looksLikeGroupingParen()) {
			advance(); // (
			grouped = true;
			if(!parseDeclaratorOps(inner, out))
				return false;
			if(!expect(TokKind::RParen, "')'"))
				return false;
		} else if(check(TokKind::Identifier)) {
			Token nameTok = advance();
			out.name = arena.make<String>(lex.text(nameTok));
			out.offset = nameTok.offset;
		}
		if(!parseDeclaratorSuffixes(ops, out.align))
			return false;
		if(failed)
			return false;
		if(grouped)
			for(const DeclOp& op : inner)
				ops.push_back(op);
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
		if(stars) {
			DeclOp op;
			op.kind = DeclOp::Kind::Pointer;
			op.count = stars;
			ops.push_back(op);
		}
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
			B32 outermost = i + 1 == ops.size();
			if(ops[i].kind != DeclOp::Kind::Func || !ops[i].func->oldStyle)
				continue;
			if(!outermost || !out.allowOldStyle) {
				fail(peek(), "parameter names (without types) in function declaration");
				return false;
			}
		}
		out.type = applyDeclOps(t, ops);
		if(!ops.empty() && ops.back().kind == DeclOp::Kind::Array) {
			out.outerArray = true;
			out.outerBound = ops.back().bound;
		}
		return true;
	}

	// the length of an array declarator: the bound as written, or the count of a typedef'd array
	// type; null when unsized
	Expr* Parser::declaredArrayLen(const DeclResult& r) {
		if(r.outerBound)
			return r.outerBound;
		U64 count = r.type.array->count;
		if(count == 0)
			return nullptr;
		Expr* len = makeExpr(ExprKind::IntLit, r.offset);
		len->intLit = {(I64)count, 32, 0};
		return len;
	}

	// splits an array type into the declarator's element type and length
	void Parser::bindDeclarator(Declarator& d, const DeclResult& r) {
		d.name = r.name;
		d.offset = r.offset;
		d.align = r.align;
		d.type = r.type;
		if(!isArrayType(r.type))
			return;
		d.isArray = true;
		d.type = r.type.array->elem;
		d.arrayLen = declaredArrayLen(r);
	}

	// declarator without a name
	B32 Parser::parseAbstractDeclarator(CType base, DeclResult& out) {
		if(!parseDeclarator(base, out))
			return false;
		if(out.name) {
			fail(peek(), "unexpected name in a type name");
			return false;
		}
		return true;
	}
} // namespace rat::cc
