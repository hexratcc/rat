#include "Ast.h"

namespace rat::cc {
	const char* exprOpName(ExprOp op) {
		switch (op) {
		case ExprOp::Pos:
			return "+";
		case ExprOp::Neg:
			return "-";
		case ExprOp::Not:
			return "!";
		case ExprOp::BitNot:
			return "~";
		case ExprOp::Add:
			return "+";
		case ExprOp::Sub:
			return "-";
		case ExprOp::Mul:
			return "*";
		case ExprOp::Div:
			return "/";
		case ExprOp::Rem:
			return "%";
		case ExprOp::Shl:
			return "<<";
		case ExprOp::Shr:
			return ">>";
		case ExprOp::Lt:
			return "<";
		case ExprOp::Gt:
			return ">";
		case ExprOp::Le:
			return "<=";
		case ExprOp::Ge:
			return ">=";
		case ExprOp::Eq:
			return "==";
		case ExprOp::Ne:
			return "!=";
		case ExprOp::BitAnd:
			return "&";
		case ExprOp::BitOr:
			return "|";
		case ExprOp::BitXor:
			return "^";
		case ExprOp::LogAnd:
			return "&&";
		case ExprOp::LogOr:
			return "||";
		case ExprOp::Assign:
			return "=";
		case ExprOp::AddAssign:
			return "+=";
		case ExprOp::SubAssign:
			return "-=";
		case ExprOp::MulAssign:
			return "*=";
		case ExprOp::DivAssign:
			return "/=";
		case ExprOp::RemAssign:
			return "%=";
		case ExprOp::ShlAssign:
			return "<<=";
		case ExprOp::ShrAssign:
			return ">>=";
		case ExprOp::AndAssign:
			return "&=";
		case ExprOp::OrAssign:
			return "|=";
		case ExprOp::XorAssign:
			return "^=";
		}
		return "?";
	}

	namespace {
		void pad(std::ostream& os, U32 depth) {
			for (U32 i = 0; i < depth; ++i)
				os << "  ";
		}

		void dumpExpr(const Expr* e, U32 depth, std::ostream& os) {
			pad(os, depth);
			switch (e->kind) {
			case ExprKind::IntLit:
				os << "int " << e->intLit.value;
				if (e->intLit.isUnsigned)
					os << "u";
				if (e->intLit.isLong)
					os << "l";
				os << "\n";
				return;
			case ExprKind::Ident:
				os << "ident " << *e->ident.name << "\n";
				return;
			case ExprKind::Unary:
				os << "unary " << exprOpName(e->unary.op) << "\n";
				dumpExpr(e->unary.operand, depth + 1, os);
				return;
			case ExprKind::Binary:
				os << (isAssignOp(e->binary.op) ? "assign " : "binary ")
					 << exprOpName(e->binary.op) << "\n";
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
			}
		}

		void dumpStmt(const Stmt* s, U32 depth, std::ostream& os) {
			pad(os, depth);
			switch (s->kind) {
			case StmtKind::Compound:
				os << "block\n";
				for (const Stmt* child : s->body)
					dumpStmt(child, depth + 1, os);
				return;
			case StmtKind::Decl:
				os << "decl\n";
				for (const Declarator& d : s->decls) {
					pad(os, depth + 1);
					os << "var " << *d.name << "\n";
					if (d.init)
						dumpExpr(d.init, depth + 2, os);
				}
				return;
			case StmtKind::Return:
				os << "return\n";
				if (s->expr)
					dumpExpr(s->expr, depth + 1, os);
				return;
			case StmtKind::Expr:
				os << "expr\n";
				dumpExpr(s->expr, depth + 1, os);
				return;
			case StmtKind::Empty:
				os << "empty\n";
				return;
			}
		}
	} // namespace

	void dumpAst(const TransUnit& unit, std::ostream& os) {
		for (const FuncDef* fn : unit.functions) {
			os << "func " << fn->name << " -> int\n";
			if (fn->body)
				dumpStmt(fn->body, 1, os);
		}
	}
} // namespace rat::cc
