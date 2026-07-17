#include "Parse/Ast.h"

namespace rat::cc {
	CType promote(CType t) {
		if(t.isVoid)
			return t;
		if(t.bits < 32)
			return CType{32, false, false};
		return t;
	}

	CType defaultArgPromote(CType t) {
		if(t.isFloat && t.ptr == 0 && t.bits < 64) {
			CType r = t;
			r.bits = 64;
			return r;
		}
		return promote(t);
	}

	CType usualArithmetic(CType a, CType b) {
		if(isComplexType(a) || isComplexType(b)) {
			CType ra = isComplexType(a) ? complexElem(a) : a;
			CType rb = isComplexType(b) ? complexElem(b) : b;
			CType r = usualArithmetic(ra, rb);
			r.isComplex = true;
			r.isFloat = true;
			return r;
		}
		if(isFloating(a) || isFloating(b)) {
			U32 bits = 64;
			if(isFloating(a) && isFloating(b))
				bits = a.bits > b.bits ? a.bits : b.bits;
			else if(isFloating(a))
				bits = a.bits;
			else
				bits = b.bits;
			CType r;
			r.bits = bits;
			r.isFloat = true;
			return r;
		}
		a = promote(a);
		b = promote(b);
		if(a.bits == b.bits && a.isUnsigned == b.isUnsigned)
			return a;
		if(a.isUnsigned == b.isUnsigned)
			return a.bits > b.bits ? a : b;
		CType u = a.isUnsigned ? a : b;
		CType s = a.isUnsigned ? b : a;
		if(u.bits >= s.bits)
			return u;
		return s;
	}

	static void appendStars(String& s, U32 ptr) {
		for(U32 i = 0; i < ptr; ++i)
			s += "*";
	}

	String typeName(CType t) {
		if(t.func) {
			String s = typeName(t.func->ret) + " (";
			appendStars(s, t.ptr);
			s += ")(";
			for(U32 i = 0; i < t.func->params.size(); ++i) {
				if(i)
					s += ", ";
				s += typeName(t.func->params[i]);
			}
			if(t.func->isVarArgs)
				s += t.func->params.empty() ? "..." : ", ...";
			else if(t.func->params.empty())
				s += "void";
			s += ")";
			return s;
		}
		if(t.array) {
			String s = typeName(t.array->elem) + "[" + std::to_string(t.array->count) + "]";
			appendStars(s, t.ptr);
			return s;
		}
		if(t.isComplex) {
			String s = (t.bits == 32 ? "float" : t.bits == 128 ? "long double" : "double");
			s += " _Complex";
			appendStars(s, t.ptr);
			return s;
		}
		if(t.strukt) {
			String s = (t.strukt->isUnion ? "union " : "struct ") + t.strukt->tag;
			appendStars(s, t.ptr);
			return s;
		}
		if(t.isVoid) {
			String s = "void";
			appendStars(s, t.ptr);
			return s;
		}
		if(t.isFloat) {
			String s = t.bits == 32 ? "float" : t.bits == 128 ? "long double" : "double";
			appendStars(s, t.ptr);
			return s;
		}
		String base;
		switch(t.bits) {
		case 1:
			return "_Bool";
		case 8:
			base = "char";
			break;
		case 16:
			base = "short";
			break;
		case 32:
			base = "int";
			break;
		default:
			base = "long";
			break;
		}
		String s = t.isUnsigned ? "unsigned " + base : base;
		appendStars(s, t.ptr);
		return s;
	}

	const char* exprOpName(ExprOp op) {
		static const char* const kNames[] = {
				"+",	"-",	"!",	"~",	 "&",		"*",	"__real__", "__imag__", "++", "--", "++",
				"--", "+",	"-",	"*",	 "/",		"%",	"<<",				">>",				"<",	">",	"<=",
				">=", "==", "!=", "&",	 "|",		"^",	"&&",				"||",				"=",	"+=", "-=",
				"*=", "/=", "%=", "<<=", ">>=", "&=", "|=",				"^=",
		};
		static_assert(sizeof(kNames) / sizeof(kNames[0]) == (U32)ExprOp::XorAssign + 1,
									"kNames must cover every ExprOp");
		return kNames[(U32)op];
	}

	namespace {
		void pad(std::ostream& os, U32 depth) {
			for(U32 i = 0; i < depth; ++i)
				os << "  ";
		}

		void dumpStmt(const Stmt* s, U32 depth, std::ostream& os);

		void dumpExpr(const Expr* e, U32 depth, std::ostream& os) {
			pad(os, depth);
			switch(e->kind) {
			case ExprKind::IntLit:
				os << "int " << e->intLit.value;
				if(e->intLit.isUnsigned)
					os << "u";
				if(e->intLit.bits == 64)
					os << (e->intLit.isLongLong ? "ll" : "l");
				os << "\n";
				return;
			case ExprKind::FloatLit:
				os << (e->floatLit.isFloat ? "float " : "double ") << e->floatLit.value << "\n";
				return;
			case ExprKind::StrLit:
				os << "str \"" << *e->str.bytes << "\"\n";
				return;
			case ExprKind::Ident:
				os << "ident " << *e->ident.name << "\n";
				return;
			case ExprKind::Call:
				os << "call " << (e->call.callee ? *e->call.callee : "(*)") << "\n";
				if(e->call.target)
					dumpExpr(e->call.target, depth + 1, os);
				for(const Expr* arg : e->args)
					dumpExpr(arg, depth + 1, os);
				return;
			case ExprKind::Cast:
				os << "cast " << typeName(e->cast.type) << "\n";
				dumpExpr(e->cast.operand, depth + 1, os);
				return;
			case ExprKind::Sizeof:
				if(e->sizeOf.operand) {
					os << "sizeof\n";
					dumpExpr(e->sizeOf.operand, depth + 1, os);
				} else {
					os << "sizeof " << typeName(e->sizeOf.type) << "\n";
				}
				return;
			case ExprKind::Unary:
				os << "unary " << exprOpName(e->unary.op) << "\n";
				dumpExpr(e->unary.operand, depth + 1, os);
				return;
			case ExprKind::Binary:
				os << (isAssignOp(e->binary.op) ? "assign " : "binary ") << exprOpName(e->binary.op)
					 << "\n";
				dumpExpr(e->binary.lhs, depth + 1, os);
				dumpExpr(e->binary.rhs, depth + 1, os);
				return;
			case ExprKind::Ternary:
				os << "ternary ?:\n";
				dumpExpr(e->ternary.cond, depth + 1, os);
				dumpExpr(e->ternary.whenTrue, depth + 1, os);
				dumpExpr(e->ternary.whenFalse, depth + 1, os);
				return;
			case ExprKind::Comma:
				os << "comma\n";
				dumpExpr(e->comma.lhs, depth + 1, os);
				dumpExpr(e->comma.rhs, depth + 1, os);
				return;
			case ExprKind::Member:
				os << "member " << (e->member.arrow ? "->" : ".") << *e->member.name << "\n";
				dumpExpr(e->member.base, depth + 1, os);
				return;
			case ExprKind::InitList:
				os << "initlist\n";
				for(const Expr* el : e->args)
					dumpExpr(el, depth + 1, os);
				return;
			case ExprKind::CompoundLit:
				os << "compound " << typeName(e->compound.type) << (e->compound.isArray ? "[]" : "")
					 << "\n";
				dumpExpr(e->compound.init, depth + 1, os);
				return;
			case ExprKind::VaArg:
				os << "va_arg " << typeName(e->vaArg.type) << "\n";
				dumpExpr(e->vaArg.ap, depth + 1, os);
				return;
			case ExprKind::StmtExpr:
				os << "stmt-expr\n";
				dumpStmt(e->stmtExpr.body, depth + 1, os);
				return;
			case ExprKind::Generic:
				os << "_Generic\n";
				dumpExpr(e->generic.control, depth + 1, os);
				for(const GenericAssoc& a : e->assocs) {
					pad(os, depth + 1);
					os << (a.isDefault ? "default" : typeName(a.type)) << ":\n";
					dumpExpr(a.result, depth + 2, os);
				}
				return;
			}
		}

		void dumpStmt(const Stmt* s, U32 depth, std::ostream& os) {
			pad(os, depth);
			switch(s->kind) {
			case StmtKind::Compound:
				os << "block\n";
				for(const Stmt* child : s->body)
					dumpStmt(child, depth + 1, os);
				return;
			case StmtKind::Decl:
				os << "decl\n";
				for(const Declarator& d : s->decls) {
					pad(os, depth + 1);
					os << "var " << typeName(d.type) << " " << *d.name << "\n";
					if(d.init)
						dumpExpr(d.init, depth + 2, os);
				}
				return;
			case StmtKind::If:
				os << "if\n";
				dumpExpr(s->expr, depth + 1, os);
				dumpStmt(s->thenBody, depth + 1, os);
				if(s->elseBody)
					dumpStmt(s->elseBody, depth + 1, os);
				return;
			case StmtKind::While:
				os << "while\n";
				dumpExpr(s->expr, depth + 1, os);
				dumpStmt(s->thenBody, depth + 1, os);
				return;
			case StmtKind::DoWhile:
				os << "do-while\n";
				dumpStmt(s->thenBody, depth + 1, os);
				dumpExpr(s->expr, depth + 1, os);
				return;
			case StmtKind::For:
				os << "for\n";
				if(s->forInit)
					dumpStmt(s->forInit, depth + 1, os);
				if(s->expr)
					dumpExpr(s->expr, depth + 1, os);
				if(s->forPost)
					dumpExpr(s->forPost, depth + 1, os);
				dumpStmt(s->thenBody, depth + 1, os);
				return;
			case StmtKind::Switch:
				os << "switch\n";
				dumpExpr(s->expr, depth + 1, os);
				dumpStmt(s->thenBody, depth + 1, os);
				return;
			case StmtKind::Case:
				os << "case\n";
				dumpExpr(s->expr, depth + 1, os);
				return;
			case StmtKind::Default:
				os << "default\n";
				return;
			case StmtKind::Break:
				os << "break\n";
				return;
			case StmtKind::Continue:
				os << "continue\n";
				return;
			case StmtKind::Return:
				os << "return\n";
				if(s->expr)
					dumpExpr(s->expr, depth + 1, os);
				return;
			case StmtKind::Expr:
				os << "expr\n";
				dumpExpr(s->expr, depth + 1, os);
				return;
			case StmtKind::Empty:
				os << "empty\n";
				return;
			case StmtKind::Label:
				os << "label " << *s->label << "\n";
				dumpStmt(s->thenBody, depth + 1, os);
				return;
			case StmtKind::Goto:
				os << "goto " << *s->label << "\n";
				return;
			}
		}
	} // namespace

	void dumpAst(const TransUnit& unit, std::ostream& os) {
		for(const Stmt* g : unit.globals)
			dumpStmt(g, 0, os);
		for(const FuncDef* fn : unit.functions) {
			os << "func " << fn->name << "(";
			for(U32 i = 0; i < fn->params.size(); ++i) {
				if(i)
					os << ", ";
				os << typeName(fn->params[i].type) << " " << *fn->params[i].name;
			}
			os << ") -> " << typeName(fn->retType) << "\n";
			if(fn->body)
				dumpStmt(fn->body, 1, os);
		}
	}
} // namespace rat::cc
