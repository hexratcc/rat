#include "IR/TextParser.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "IR/Opcode.h"
#include "IR/Type.h"

#include <cctype>
#include <sstream>

namespace rat {
	namespace {
		String stripAnsi(const String& s) {
			String out;
			out.reserve(s.size());
			for (U32 i = 0; i < s.size();) {
				if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
					i += 2;
					while (i < s.size() && s[i] != 'm')
						++i;
					if (i < s.size())
						++i;
				} else {
					out.push_back(s[i++]);
				}
			}
			return out;
		}

		String ltrim(const String& s) {
			U32 i = 0;
			while (i < s.size() && std::isspace((U8)s[i]))
				++i;
			return s.substr(i);
		}
		String rtrim(const String& s) {
			U32 e = (U32)s.size();
			while (e > 0 && std::isspace((U8)s[e - 1]))
				--e;
			return s.substr(0, e);
		}
		String trim(const String& s) { return rtrim(ltrim(s)); }

		B32 allDigits(const String& s) {
			if (s.empty())
				return false;
			for (char c : s)
				if (!std::isdigit((U8)c))
					return false;
			return true;
		}

		List<U32> parseVRefs(const String& s) {
			List<U32> out;
			U32 i = 0;
			while (i < s.size()) {
				if (s[i] == 'v' && i + 1 < s.size() && std::isdigit((U8)s[i + 1])) {
					U32 j = i + 1;
					while (j < s.size() && std::isdigit((U8)s[j]))
						++j;
					out.push_back((U32)std::stoul(s.substr(i + 1, j - i - 1)));
					i = j;
				} else {
					++i;
				}
			}
			return out;
		}

		Opcode opcodeForMnemonic(const String& m, B32& ok) {
			for (i32 i = (i32)Opcode::Start; i <= (i32)Opcode::Call; ++i) {
				Opcode op = (Opcode)i;
				if (m == getOpcodeMnemonic(op)) {
					ok = true;
					return op;
				}
			}
			ok = false;
			return Opcode::Start;
		}

		struct Rec {
			U32 id = 0;
			Opcode op = Opcode::Start;
			Type* ty = nullptr;
			I64 cval = 0; // Constant
			U32 projIndex = 0; // Proj
			String projLabel; // Proj
			String callee; // Call
			B32 loopHeader = false; // Region
			List<U32> operands;
		};

		struct Parser {
			Module& mod;
			std::ostream& err;
			U32 lineNo = 0;
			B32 failed = false;

			Parser(Module& mod, std::ostream& err) : mod(mod), err(err) {}

			B32 fail(const String& msg) {
				err << "parse error (line " << lineNo << "): " << msg << "\n";
				failed = true;
				return false;
			}

			// skip blank lines and ';' comments
			B32 skip(const String& t) { return t.empty() || t[0] == ';'; }

			B32 parse(std::istream& in) {
				String line;
				while (std::getline(in, line)) {
					++lineNo;
					String t = trim(stripAnsi(line));
					if (skip(t))
						continue;
					if (t.rfind("func ", 0) == 0) {
						if (!parseFunction(t, in))
							return false;
					} else {
						return fail("expected a 'func' header, got: " + t);
					}
				}
				return !failed;
			}

			Type* parseType(const String& s) {
				String t = trim(s);
				if (t.empty()) {
					fail("empty type");
					return nullptr;
				}
				if (t.front() == '(') {
					if (t.back() != ')') {
						fail("unbalanced tuple type: " + t);
						return nullptr;
					}
					String inner = t.substr(1, t.size() - 2);
					List<Type*> elems;
					i32 depth = 0;
					String cur;
					auto flush = [&]() -> B32 {
						String e = trim(cur);
						cur.clear();
						if (e.empty())
							return true;
						Type* et = parseType(e);
						if (!et)
							return false;
						elems.push_back(et);
						return true;
					};
					for (char c : inner) {
						if (c == '(')
							++depth;
						if (c == ')')
							--depth;
						if (c == ',' && depth == 0) {
							if (!flush())
								return nullptr;
						} else {
							cur.push_back(c);
						}
					}
					if (!flush())
						return nullptr;
					return mod.getTuple(elems);
				}
				if (t == "ctrl")
					return mod.getControl();
				if (t == "mem")
					return mod.getMemory();
				if (t == "ptr")
					return mod.getPtr();
				if (t.size() >= 2 && t[0] == 'i' && allDigits(t.substr(1)))
					return mod.getInt((U32)std::stoul(t.substr(1)));
				fail("unknown type '" + t + "'");
				return nullptr;
			}

			B32 parseFunction(const String& header, std::istream& in) {
				size_t lp = header.find('(');
				size_t rp = header.rfind(')');
				size_t arrow = header.find("->");
				size_t brace = header.rfind('{');
				if (lp == String::npos || rp == String::npos || rp < lp ||
						arrow == String::npos || brace == String::npos)
					return fail("malformed func header: " + header);

				String name = trim(header.substr(5, lp - 5));
				String paramsStr = header.substr(lp + 1, rp - lp - 1);
				String retStr = trim(header.substr(arrow + 2, brace - (arrow + 2)));

				List<Type*> params;
				{
					i32 depth = 0;
					String cur;
					auto flush = [&]() -> B32 {
						String p = trim(cur);
						cur.clear();
						if (p.empty())
							return true;
						Type* pt = parseType(p);
						if (!pt)
							return false;
						params.push_back(pt);
						return true;
					};
					for (char c : paramsStr) {
						if (c == '(')
							++depth;
						if (c == ')')
							--depth;
						if (c == ',' && depth == 0) {
							if (!flush())
								return false;
						} else {
							cur.push_back(c);
						}
					}
					if (!flush())
						return false;
				}

				Type* ret = nullptr;
				if (retStr != "void") {
					ret = parseType(retStr);
					if (!ret)
						return false;
				}

				Function* fn = mod.createFunction(name, params, ret);

				List<Rec> recs;
				String line;
				B32 closed = false;
				while (std::getline(in, line)) {
					++lineNo;
					String t = trim(stripAnsi(line));
					if (t == "}") {
						closed = true;
						break;
					}
					if (skip(t))
						continue;
					Rec r;
					if (!parseNodeLine(t, r))
						return false;
					recs.push_back(std::move(r));
				}
				if (!closed)
					return fail("missing '}' to close function " + name);

				return build(fn, recs);
			}

			B32 parseNodeLine(const String& line, Rec& r) {
				size_t eq = line.find(" = ");
				if (eq == String::npos)
					return fail("expected ' = ' in: " + line);
				String lhs = trim(line.substr(0, eq));
				if (lhs.empty() || lhs[0] != 'v' || !allDigits(lhs.substr(1)))
					return fail("bad result name '" + lhs + "'");
				r.id = (U32)std::stoul(lhs.substr(1));

				String rest = line.substr(eq + 3);
				size_t colon = rest.find(" : ");
				if (colon == String::npos)
					return fail("expected ' : ' in: " + line);
				String mnem = trim(rest.substr(0, colon));
				B32 ok = false;
				r.op = opcodeForMnemonic(mnem, ok);
				if (!ok)
					return fail("unknown mnemonic '" + mnem + "'");

				// split the type (possibly a parenthesized tuple) from the remainder
				String after = ltrim(rest.substr(colon + 3));
				String typeStr, remainder;
				if (!after.empty() && after.front() == '(') {
					i32 depth = 0;
					U32 i = 0;
					for (; i < after.size(); ++i) {
						if (after[i] == '(')
							++depth;
						else if (after[i] == ')') {
							--depth;
							if (depth == 0) {
								++i;
								break;
							}
						}
					}
					typeStr = after.substr(0, i);
					remainder = after.substr(i);
				} else {
					size_t sp = after.find(' ');
					if (sp == String::npos) {
						typeStr = after;
						remainder = "";
					} else {
						typeStr = after.substr(0, sp);
						remainder = after.substr(sp);
					}
				}
				r.ty = parseType(typeStr);
				if (!r.ty)
					return false;

				remainder = trim(remainder);

				switch (r.op) {
				case Opcode::Constant: {
					if (remainder.empty())
						return fail("constant is missing its value: " + line);
					try {
						r.cval = (I64)std::stoll(remainder);
					} catch (...) {
						return fail("bad constant value '" + remainder + "'");
					}
					break;
				}
				case Opcode::Proj: {
					std::istringstream ss(remainder);
					String tok;
					List<String> toks;
					while (ss >> tok)
						toks.push_back(tok);
					if (toks.empty() || toks[0].empty() || toks[0][0] != '#')
						return fail("malformed proj (expected #index): " + line);
					if (!allDigits(toks[0].substr(1)))
						return fail("bad proj index '" + toks[0] + "'");
					r.projIndex = (U32)std::stoul(toks[0].substr(1));
					U32 k = 1;
					if (k < toks.size() && !toks[k].empty() && toks[k].front() == '"') {
						String l = toks[k];
						if (l.size() >= 2 && l.back() == '"')
							r.projLabel = l.substr(1, l.size() - 2);
						++k;
					}
					List<U32> refs = parseVRefs(remainder);
					if (refs.size() != 1)
						return fail("proj must reference exactly one producer: " + line);
					r.operands = refs;
					break;
				}
				case Opcode::Call: {
					size_t q1 = remainder.find('"');
					size_t q2 =
							(q1 == String::npos) ? String::npos : remainder.find('"', q1 + 1);
					if (q1 == String::npos || q2 == String::npos)
						return fail("call is missing its quoted callee: " + line);
					r.callee = remainder.substr(q1 + 1, q2 - q1 - 1);
					r.operands = parseVRefs(remainder.substr(q2 + 1));
					break;
				}
				case Opcode::Region: {
					String body = remainder;
					std::istringstream ss(body);
					String first;
					ss >> first;
					if (first == "loop") {
						r.loopHeader = true;
						size_t pos = body.find("loop");
						body = body.substr(pos + 4);
					}
					r.operands = parseVRefs(body);
					break;
				}
				default:
					r.operands = parseVRefs(remainder);
					break;
				}
				return true;
			}

			B32 build(Function* fn, const List<Rec>& recs) {
				Map<U32, Node*> byId;

				// pass 1
				Node* startCtrl = nullptr;
				Node* startMem = nullptr;
				for (Node* u : fn->getStart()->getUsers()) {
					if (u->getOpcode() != Opcode::Proj)
						continue;
					U32 idx = static_cast<ProjNode*>(u)->getIndex();
					if (idx == StartNode::controlProjIndex())
						startCtrl = u;
					else if (idx == StartNode::memoryProjIndex())
						startMem = u;
				}

				for (const Rec& r : recs) {
					if (r.op == Opcode::Start)
						byId[r.id] = fn->getStart();
					else if (r.op == Opcode::Stop)
						byId[r.id] = fn->getStop();
				}

				auto need = [&](const Rec& r, U32 i) -> Node* {
					if (i >= r.operands.size()) {
						fail("v" + std::to_string(r.id) + " (" + getOpcodeMnemonic(r.op) +
								 ") is missing operand " + std::to_string(i));
						return nullptr;
					}
					auto it = byId.find(r.operands[i]);
					if (it == byId.end()) {
						fail("v" + std::to_string(r.id) + " references undefined v" +
								 std::to_string(r.operands[i]));
						return nullptr;
					}
					return it->second;
				};

				List<B32> done(recs.size(), false);
				for (U32 k = 0; k < recs.size(); ++k)
					if (recs[k].op == Opcode::Start || recs[k].op == Opcode::Stop)
						done[k] = true;

				auto ready = [&](const Rec& r) -> B32 {
					if (r.op == Opcode::Region || r.op == Opcode::Phi)
						return true;
					for (U32 v : r.operands)
						if (byId.find(v) == byId.end())
							return false;
					return true;
				};

				U32 remaining = 0;
				for (B32 d : done)
					if (!d)
						++remaining;

				B32 progress = true;
				while (remaining && progress) {
					progress = false;
					for (U32 k = 0; k < recs.size(); ++k) {
						if (done[k])
							continue;
						const Rec& r = recs[k];
						if (!ready(r))
							continue;
						Node* n = nullptr;
						Opcode op = r.op;
						if (op == Opcode::Region) {
							auto* reg = fn->create<RegionNode>(r.ty, List<Node*>{});
							reg->setLoopHeader(r.loopHeader);
							n = reg;
						} else if (op == Opcode::Phi) {
							n = fn->create<PhiNode>(r.ty, List<Node*>{});
						} else if (op == Opcode::Constant) {
							n = fn->create<ConstantNode>(r.ty, r.cval);
						} else if (op == Opcode::If) {
							Node* c = need(r, 0);
							Node* p = need(r, 1);
							if (!c || !p)
								return false;
							n = fn->create<IfNode>(r.ty, c, p);
						} else if (op == Opcode::Proj) {
							Node* prod = need(r, 0);
							if (!prod)
								return false;
							if (prod == fn->getStart() &&
									r.projIndex == StartNode::controlProjIndex() && startCtrl)
								n = startCtrl;
							else if (prod == fn->getStart() &&
											 r.projIndex == StartNode::memoryProjIndex() && startMem)
								n = startMem;
							else
								n = fn->create<ProjNode>(r.ty, prod, r.projIndex, r.projLabel);
						} else if (op == Opcode::Load) {
							Node* c = need(r, 0);
							Node* m = need(r, 1);
							Node* ptr = need(r, 2);
							if (!c || !m || !ptr)
								return false;
							n = fn->create<LoadNode>(r.ty, c, m, ptr);
						} else if (op == Opcode::Store) {
							Node* c = need(r, 0);
							Node* m = need(r, 1);
							Node* ptr = need(r, 2);
							Node* v = need(r, 3);
							if (!c || !m || !ptr || !v)
								return false;
							n = fn->create<StoreNode>(r.ty, c, m, ptr, v);
						} else if (op == Opcode::Call) {
							List<Node*> ins;
							for (U32 i = 0; i < r.operands.size(); ++i) {
								Node* a = need(r, i);
								if (!a)
									return false;
								ins.push_back(a);
							}
							B32 rv = r.ty->isTuple() && r.ty->getTupleElementCount() == 3;
							n = fn->create<CallNode>(r.ty, r.callee, rv, ins);
						} else if (op == Opcode::Return) {
							List<Node*> ins;
							for (U32 i = 0; i < r.operands.size(); ++i) {
								Node* a = need(r, i);
								if (!a)
									return false;
								ins.push_back(a);
							}
							n = fn->create<ReturnNode>(r.ty, ins);
						} else if (isBinaryOpcode(op)) {
							Node* l = need(r, 0);
							Node* rh = need(r, 1);
							if (!l || !rh)
								return false;
							n = fn->create<BinaryNode>(op, r.ty, l, rh);
						} else if (isUnaryOpcode(op)) {
							Node* v = need(r, 0);
							if (!v)
								return false;
							n = fn->create<UnaryNode>(op, r.ty, v);
						} else if (isCompareOpcode(op)) {
							Node* l = need(r, 0);
							Node* rh = need(r, 1);
							if (!l || !rh)
								return false;
							n = fn->create<CompareNode>(op, r.ty, l, rh);
						} else if (isConvertOpcode(op)) {
							Node* v = need(r, 0);
							if (!v)
								return false;
							n = fn->create<ConvertNode>(op, r.ty, v);
						} else {
							return fail(String("cannot construct opcode '") +
													getOpcodeMnemonic(op) + "'");
						}
						byId[r.id] = n;
						done[k] = true;
						--remaining;
						progress = true;
					}
				}

				if (remaining) {
					for (U32 k = 0; k < recs.size(); ++k)
						if (!done[k])
							return fail("v" + std::to_string(recs[k].id) +
													" has unresolved operands (undefined or cyclic ref)");
				}

				// pass 2
				for (const Rec& r : recs) {
					if (r.op != Opcode::Region && r.op != Opcode::Phi &&
							r.op != Opcode::Stop)
						continue;
					Node* n = byId[r.id];
					for (U32 i = 0; i < r.operands.size(); ++i) {
						Node* in = need(r, i);
						if (!in)
							return false;
						n->addInput(in);
					}
				}
				return true;
			}
		};
	} // namespace

	B32 parseText(std::istream& in, Module& module, std::ostream& errors) {
		return Parser(module, errors).parse(in);
	}

	B32 parseText(const String& text, Module& module, std::ostream& errors) {
		std::istringstream ss(text);
		return parseText(ss, module, errors);
	}
} // namespace rat
