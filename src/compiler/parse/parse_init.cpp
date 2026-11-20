#include "parse/parser.h"

namespace rat::cc {
	// . name | '[' const-expr [ '...' const-expr ] ']'
	// a '...' range is allowed on the first designator only
	B32 Parser::parseDesignator(Designator& cur, B32 first, I64& rangeEnd) {
		if(accept(TokKind::Dot)) {
			if(!check(TokKind::Identifier))
				return fail(peek(), "expected a field name after '.'");
			Token id = advance();
			cur.isIndex = false;
			cur.field = arena.make<String>(lex.text(id));
			return true;
		}
		advance(); // '['
		Expr* ix = parseAssignment();
		I64 v = 0;
		if(!ix || !evalIntConst(ix, v) || v < 0)
			return fail(peek(), "array designator must be a non-negative integer constant");
		cur.isIndex = true;
		cur.index = v;
		if(first && accept(TokKind::Ellipsis)) {
			Expr* hi = parseAssignment();
			I64 hv = 0;
			if(!hi || !evalIntConst(hi, hv) || hv < v)
				return fail(peek(), "invalid range designator");
			rangeEnd = hv;
		}
		return expect(TokKind::RBracket);
	}

	// designator... =
	B32 Parser::parseDesignation(Designator& des, I64& rangeEnd) {
		Designator* cur = &des;
		for(B32 first = true;; first = false) {
			if(!parseDesignator(*cur, first, rangeEnd))
				return false;
			if(!check(TokKind::Dot) && !check(TokKind::LBracket))
				break;
			Designator* nxt = arena.make<Designator>();
			nxt->isSet = true;
			cur->next = nxt;
			cur = nxt;
		}
		if(!expect(TokKind::Assign, "'=' after designator"))
			return false;
		des.isSet = true;
		return true;
	}

	// [designation] initializer
	B32 Parser::parseInitElement(Expr* list) {
		Designator des;
		I64 rangeEnd = -1; // GNU range designator [a ... b]
		if((check(TokKind::Dot) || check(TokKind::LBracket)) && !parseDesignation(des, rangeEnd))
			return false;
		Expr* el = parseInitializer();
		if(!el)
			return false;
		if(rangeEnd < des.index) {
			list->args.push_back(el);
			list->designators.push_back(des);
			return true;
		}
		for(I64 k = des.index; k <= rangeEnd; ++k) {
			list->args.push_back(el);
			list->designators.push_back({true, true, k});
		}
		return true;
	}

	// { [ element [, element]... [,] ] } | assignment
	Expr* Parser::parseInitializer() {
		if(!check(TokKind::LBrace))
			return parseAssignment();
		Token lb = advance(); // {
		Expr* e = makeExpr(ExprKind::InitList, lb.offset);
		while(!check(TokKind::RBrace)) {
			if(!parseInitElement(e))
				return nullptr;
			if(!accept(TokKind::Comma))
				break;
		}
		if(!expect(TokKind::RBrace))
			return nullptr;
		return e;
	}

	// an integer constant expression, anything else is a diagnostic
	B32 Parser::evalIntConst(const Expr* e, I64& out) {
		if(ConstEval(lay).evalInt(e, out))
			return true;
		return fail(peek(), "expected an integer constant expression");
	}
} // namespace rat::cc
