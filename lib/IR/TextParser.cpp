#include "IR/TextParser.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "IR/Opcode.h"
#include "IR/Type.h"
#include "Support/StringUtil.h"

namespace rat {
	namespace detail {
		B32 allDigits(const String& s) {
			if(s.empty())
				return false;
			for(C8 c : s)
				if(!std::isdigit((U8)c))
					return false;
			return true;
		}

		List<U32> parseVRefs(const String& s) {
			List<U32> out;
			U32 i = 0;
			while(i < s.size()) {
				if(s[i] == 'v' && i + 1 < s.size() && std::isdigit((U8)s[i + 1])) {
					U32 j = i + 1;
					while(j < s.size() && std::isdigit((U8)s[j]))
						++j;
					out.push_back((U32)std::stoul(s.substr(i + 1, j - i - 1)));
					i = j;
				} else {
					++i;
				}
			}
			return out;
		}

		B32 unquoteBytes(const String& s, List<U8>& out) {
			String t = trim(s);
			if(t.size() < 2 || t.front() != '"' || t.back() != '"')
				return false;
			auto hexVal = [](C8 c, U8& v) -> B32 {
				if(c >= '0' && c <= '9')
					v = (U8)(c - '0');
				else if(c >= 'a' && c <= 'f')
					v = (U8)(c - 'a' + 10);
				else if(c >= 'A' && c <= 'F')
					v = (U8)(c - 'A' + 10);
				else
					return false;
				return true;
			};
			for(U32 i = 1; i + 1 < t.size();) {
				if(t[i] == '\\') {
					if(i + 3 >= t.size())
						return false;
					U8 hi, lo;
					if(!hexVal(t[i + 1], hi) || !hexVal(t[i + 2], lo))
						return false;
					out.push_back((U8)((hi << 4) | lo));
					i += 3;
				} else {
					out.push_back((U8)t[i]);
					++i;
				}
			}
			return true;
		}

		Opcode opcodeForMnemonic(const String& m, B32& ok) {
			for(U32 i = (U32)Opcode::Start; i <= (U32)Opcode::Alloc; ++i) {
				Opcode op = (Opcode)i;
				if(m == getOpcodeMnemonic(op)) {
					ok = true;
					return op;
				}
			}
			ok = false;
			return Opcode::Start;
		}

		Parser::Parser(Module& mod, std::ostream& err)
		: mod(mod),
			err(err) {}

		B32 Parser::fail(const String& msg) {
			err << "parse error (line " << lineNo << "): " << msg << "\n";
			failed = true;
			return false;
		}

		B32 Parser::skip(const String& t) { return t.empty() || t[0] == ';'; }

		B32 Parser::parse(std::istream& in) {
			String line;
			while(std::getline(in, line)) {
				++lineNo;
				String t = trim(stripAnsi(line));
				if(skip(t))
					continue;
				if(t.rfind("func ", 0) == 0) {
					if(!parseFunction(t, in))
						return false;
				} else if(t.rfind("const ", 0) == 0 || t.rfind("var ", 0) == 0) {
					if(!parseGlobal(t))
						return false;
				} else {
					return fail("expected a 'func', 'const' or 'var', got: " + t);
				}
			}
			return !failed;
		}

		Type* Parser::parseType(const String& s) {
			String t = trim(s);
			if(t.empty()) {
				fail("empty type");
				return nullptr;
			}
			if(t.front() == '(') {
				if(t.back() != ')') {
					fail("unbalanced tuple type: " + t);
					return nullptr;
				}
				String inner = t.substr(1, t.size() - 2);
				List<Type*> elems;
				U32 depth = 0;
				String cur;
				auto flush = [&]() -> B32 {
					String e = trim(cur);
					cur.clear();
					if(e.empty())
						return true;
					Type* et = parseType(e);
					if(!et)
						return false;
					elems.push_back(et);
					return true;
				};
				for(C8 c : inner) {
					if(c == '(')
						++depth;
					if(c == ')')
						--depth;
					if(c == ',' && depth == 0) {
						if(!flush())
							return nullptr;
					} else {
						cur.push_back(c);
					}
				}
				if(!flush())
					return nullptr;
				return mod.getTuple(elems);
			}
			if(t.front() == '[') {
				if(t.back() != ']') {
					fail("unbalanced array type: " + t);
					return nullptr;
				}
				String inner = t.substr(1, t.size() - 2);
				U64 x = inner.find(" x ");
				if(x == String::npos) {
					fail("array type must be '[N x T]': " + t);
					return nullptr;
				}
				String countStr = trim(inner.substr(0, x));
				if(!allDigits(countStr)) {
					fail("bad array count in: " + t);
					return nullptr;
				}
				Type* elem = parseType(inner.substr(x + 3));
				if(!elem)
					return nullptr;
				return mod.getArray(elem, (U32)std::stoul(countStr));
			}
			if(t == "ctrl")
				return mod.getControl();
			if(t == "mem")
				return mod.getMemory();
			if(t == "ptr")
				return mod.getPtr();
			if(t.size() >= 2 && t[0] == 'i' && allDigits(t.substr(1)))
				return mod.getInt((U32)std::stoul(t.substr(1)));
			fail("unknown type '" + t + "'");
			return nullptr;
		}

		B32 Parser::parseGlobal(const String& line) {
			B32 isConst = line.rfind("const ", 0) == 0;
			String rest = trim(line.substr(isConst ? 6 : 4));
			U64 colon = rest.find(" : ");
			U64 eq = rest.find(" = ");
			if(colon == String::npos || eq == String::npos || eq < colon)
				return fail("malformed global (want NAME : TYPE = \"...\"): " + line);
			String name = trim(rest.substr(0, colon));
			String typeStr = trim(rest.substr(colon + 3, eq - (colon + 3)));
			String initStr = trim(rest.substr(eq + 3));
			if(name.empty())
				return fail("global has no name: " + line);
			Type* ty = parseType(typeStr);
			if(!ty)
				return false;
			List<U8> init;
			if(!unquoteBytes(initStr, init))
				return fail("malformed global initializer: " + initStr);
			mod.createGlobal(name, ty, isConst, std::move(init));
			return true;
		}

		B32 Parser::parseFunction(const String& header, std::istream& in) {
			U64 lp = header.find('(');
			U64 rp = header.rfind(')');
			U64 arrow = header.find("->");
			U64 brace = header.rfind('{');
			if(lp == String::npos || rp == String::npos || rp < lp || arrow == String::npos ||
				 brace == String::npos)
				return fail("malformed func header: " + header);

			String name = trim(header.substr(5, lp - 5));
			String paramsStr = header.substr(lp + 1, rp - lp - 1);
			String retStr = trim(header.substr(arrow + 2, brace - (arrow + 2)));

			List<Type*> params;
			{
				U32 depth = 0;
				String cur;
				auto flush = [&]() -> B32 {
					String p = trim(cur);
					cur.clear();
					if(p.empty())
						return true;
					Type* pt = parseType(p);
					if(!pt)
						return false;
					params.push_back(pt);
					return true;
				};
				for(C8 c : paramsStr) {
					if(c == '(')
						++depth;
					if(c == ')')
						--depth;
					if(c == ',' && depth == 0) {
						if(!flush())
							return false;
					} else {
						cur.push_back(c);
					}
				}
				if(!flush())
					return false;
			}

			Type* ret = nullptr;
			if(retStr != "void") {
				ret = parseType(retStr);
				if(!ret)
					return false;
			}

			Function* fn = mod.createFunction(name, params, ret);

			List<ParsedNode> parsed;
			String line;
			B32 closed = false;
			while(std::getline(in, line)) {
				++lineNo;
				String t = trim(stripAnsi(line));
				if(t == "}") {
					closed = true;
					break;
				}
				if(skip(t))
					continue;
				ParsedNode p;
				if(!parseNodeLine(t, p))
					return false;
				parsed.push_back(std::move(p));
			}
			if(!closed)
				return fail("missing '}' to close function " + name);

			return build(fn, parsed);
		}

		B32 Parser::parseNodeLine(const String& line, ParsedNode& pn) {
			U64 eq = line.find(" = ");
			if(eq == String::npos)
				return fail("expected ' = ' in: " + line);
			String lhs = trim(line.substr(0, eq));
			if(lhs.empty() || lhs[0] != 'v' || !allDigits(lhs.substr(1)))
				return fail("bad result name '" + lhs + "'");
			pn.id = (U32)std::stoul(lhs.substr(1));

			String rest = line.substr(eq + 3);
			U64 colon = rest.find(" : ");
			if(colon == String::npos)
				return fail("expected ' : ' in: " + line);
			String mnem = trim(rest.substr(0, colon));
			B32 ok = false;
			pn.op = opcodeForMnemonic(mnem, ok);
			if(!ok)
				return fail("unknown mnemonic '" + mnem + "'");

			// split the type (possibly a parenthesized tuple) from the remainder
			String after = ltrim(rest.substr(colon + 3));
			String typeStr, remainder;
			if(!after.empty() && after.front() == '(') {
				U32 depth = 0;
				U32 i = 0;
				for(; i < after.size(); ++i) {
					if(after[i] == '(')
						++depth;
					else if(after[i] == ')') {
						--depth;
						if(depth == 0) {
							++i;
							break;
						}
					}
				}
				typeStr = after.substr(0, i);
				remainder = after.substr(i);
			} else {
				U64 sp = after.find(' ');
				if(sp == String::npos) {
					typeStr = after;
					remainder = "";
				} else {
					typeStr = after.substr(0, sp);
					remainder = after.substr(sp);
				}
			}
			pn.ty = parseType(typeStr);
			if(!pn.ty)
				return false;

			remainder = trim(remainder);

			switch(pn.op) {
			case Opcode::Constant: {
				if(remainder.empty())
					return fail("constant is missing its value: " + line);
				try {
					pn.cval = (I64)std::stoll(remainder);
				} catch(...) {
					return fail("bad constant value '" + remainder + "'");
				}
				break;
			}
			case Opcode::Proj: {
				std::istringstream ss(remainder);
				String tok;
				List<String> toks;
				while(ss >> tok)
					toks.push_back(tok);
				if(toks.empty() || toks[0].empty() || toks[0][0] != '#')
					return fail("malformed proj (expected #index): " + line);
				if(!allDigits(toks[0].substr(1)))
					return fail("bad proj index '" + toks[0] + "'");
				pn.projIndex = (U32)std::stoul(toks[0].substr(1));
				U32 k = 1;
				if(k < toks.size() && !toks[k].empty() && toks[k].front() == '"') {
					String l = toks[k];
					if(l.size() >= 2 && l.back() == '"')
						pn.projLabel = l.substr(1, l.size() - 2);
					++k;
				}
				List<U32> refs = parseVRefs(remainder);
				if(refs.size() != 1)
					return fail("proj must reference exactly one producer: " + line);
				pn.operands = refs;
				break;
			}
			case Opcode::Call: {
				U64 q1 = remainder.find('"');
				U64 q2 = (q1 == String::npos) ? String::npos : remainder.find('"', q1 + 1);
				if(q1 == String::npos || q2 == String::npos)
					return fail("call is missing its quoted callee: " + line);
				pn.callee = remainder.substr(q1 + 1, q2 - q1 - 1);
				pn.operands = parseVRefs(remainder.substr(q2 + 1));
				break;
			}
			case Opcode::Global: {
				U64 q1 = remainder.find('"');
				U64 q2 = (q1 == String::npos) ? String::npos : remainder.find('"', q1 + 1);
				if(q1 == String::npos || q2 == String::npos)
					return fail("global node is missing its quoted symbol: " + line);
				pn.symbol = remainder.substr(q1 + 1, q2 - q1 - 1);
				break;
			}
			case Opcode::Alloc: {
				if(remainder.empty())
					return fail("alloc node is missing its type: " + line);
				pn.allocType = parseType(remainder);
				if(!pn.allocType)
					return false;
				break;
			}
			case Opcode::Region: {
				String body = remainder;
				std::istringstream ss(body);
				String first;
				ss >> first;
				if(first == "loop") {
					pn.loopHeader = true;
					U64 pos = body.find("loop");
					body = body.substr(pos + 4);
				}
				pn.operands = parseVRefs(body);
				break;
			}
			default:
				pn.operands = parseVRefs(remainder);
				break;
			}
			return true;
		}

		Node* Parser::operand(const ParsedNode& pn, U32 index) {
			if(index >= pn.operands.size()) {
				fail("v" + std::to_string(pn.id) + " (" + getOpcodeMnemonic(pn.op) +
						 ") is missing operand " + std::to_string(index));
				return nullptr;
			}
			auto it = byId.find(pn.operands[index]);
			if(it == byId.end()) {
				fail("v" + std::to_string(pn.id) + " references undefined v" +
						 std::to_string(pn.operands[index]));
				return nullptr;
			}
			return it->second;
		}

		void Parser::seedStartStop(Function* fn, const List<ParsedNode>& nodes) {
			startCtrl = nullptr;
			startMem = nullptr;
			for(Node* u : fn->getStart()->getUsers()) {
				ProjNode* p = dyn_cast<ProjNode>(u);
				if(!p)
					continue;
				if(p->getIndex() == StartNode::controlProjIndex())
					startCtrl = u;
				else if(p->getIndex() == StartNode::memoryProjIndex())
					startMem = u;
			}

			for(const ParsedNode& pn : nodes) {
				if(pn.op == Opcode::Start)
					byId[pn.id] = fn->getStart();
				else if(pn.op == Opcode::Stop)
					byId[pn.id] = fn->getStop();
			}
		}

		// constructs the single node described by `pn`, looking up its operands via
		// operand(). Region / Phi are created with no inputs (wired later by
		// wireDeferredInputs); returns null on a missing operand or unknown opcode.
		Node* Parser::makeNode(Function* fn, const ParsedNode& pn) {
			Opcode op = pn.op;
			if(op == Opcode::Region) {
				auto* reg = fn->create<RegionNode>(pn.ty, List<Node*>{});
				reg->setLoopHeader(pn.loopHeader);
				return reg;
			}
			if(op == Opcode::Phi)
				return fn->create<PhiNode>(pn.ty, List<Node*>{});
			if(op == Opcode::Constant)
				return fn->create<ConstantNode>(pn.ty, pn.cval);
			if(op == Opcode::If) {
				Node* c = operand(pn, 0);
				Node* p = operand(pn, 1);
				if(!c || !p)
					return nullptr;
				return fn->create<IfNode>(pn.ty, c, p);
			}
			if(op == Opcode::Proj) {
				Node* prod = operand(pn, 0);
				if(!prod)
					return nullptr;
				if(prod == fn->getStart() && pn.projIndex == StartNode::controlProjIndex() && startCtrl)
					return startCtrl;
				if(prod == fn->getStart() && pn.projIndex == StartNode::memoryProjIndex() && startMem)
					return startMem;
				return fn->create<ProjNode>(pn.ty, prod, pn.projIndex, pn.projLabel);
			}
			if(op == Opcode::Load) {
				Node* c = operand(pn, 0);
				Node* m = operand(pn, 1);
				Node* ptr = operand(pn, 2);
				if(!c || !m || !ptr)
					return nullptr;
				return fn->create<LoadNode>(pn.ty, c, m, ptr);
			}
			if(op == Opcode::Store) {
				Node* c = operand(pn, 0);
				Node* m = operand(pn, 1);
				Node* ptr = operand(pn, 2);
				Node* v = operand(pn, 3);
				if(!c || !m || !ptr || !v)
					return nullptr;
				return fn->create<StoreNode>(pn.ty, c, m, ptr, v);
			}
			if(op == Opcode::Call || op == Opcode::Return) {
				List<Node*> ins;
				for(U32 i = 0; i < pn.operands.size(); ++i) {
					Node* a = operand(pn, i);
					if(!a)
						return nullptr;
					ins.push_back(a);
				}
				if(op == Opcode::Return)
					return fn->create<ReturnNode>(pn.ty, ins);
				B32 rv = pn.ty->isTuple() && pn.ty->getTupleElementCount() == 3;
				return fn->create<CallNode>(pn.ty, pn.callee, rv, ins);
			}
			if(isBinaryOpcode(op) || isCompareOpcode(op)) {
				Node* l = operand(pn, 0);
				Node* rh = operand(pn, 1);
				if(!l || !rh)
					return nullptr;
				if(isBinaryOpcode(op))
					return fn->create<BinaryNode>(op, pn.ty, l, rh);
				return fn->create<CompareNode>(op, pn.ty, l, rh);
			}
			if(isUnaryOpcode(op) || isConvertOpcode(op)) {
				Node* v = operand(pn, 0);
				if(!v)
					return nullptr;
				if(isUnaryOpcode(op))
					return fn->create<UnaryNode>(op, pn.ty, v);
				return fn->create<ConvertNode>(op, pn.ty, v);
			}
			if(op == Opcode::Global)
				return fn->create<GlobalNode>(pn.ty, pn.symbol);
			if(op == Opcode::Alloc)
				return fn->create<AllocNode>(pn.ty, pn.allocType);
			fail(String("cannot construct opcode '") + getOpcodeMnemonic(op) + "'");
			return nullptr;
		}

		B32 Parser::materialize(Function* fn, const List<ParsedNode>& nodes) {
			// a record is ready once every operand it references already exists;
			// Region / Phi are exempt since their inputs are wired in a later pass
			auto ready = [&](const ParsedNode& pn) -> B32 {
				if(pn.op == Opcode::Region || pn.op == Opcode::Phi)
					return true;
				for(U32 v : pn.operands)
					if(byId.find(v) == byId.end())
						return false;
				return true;
			};

			List<B32> done(nodes.size(), false);
			U32 remaining = 0;
			for(U32 k = 0; k < nodes.size(); ++k) {
				if(nodes[k].op == Opcode::Start || nodes[k].op == Opcode::Stop)
					done[k] = true; // seeded by seedStartStop
				else
					++remaining;
			}

			B32 progress = true;
			while(remaining && progress) {
				progress = false;
				for(U32 k = 0; k < nodes.size(); ++k) {
					if(done[k] || !ready(nodes[k]))
						continue;
					Node* n = makeNode(fn, nodes[k]);
					if(!n)
						return false;
					byId[nodes[k].id] = n;
					done[k] = true;
					--remaining;
					progress = true;
				}
			}

			if(remaining) {
				for(U32 k = 0; k < nodes.size(); ++k)
					if(!done[k])
						return fail("v" + std::to_string(nodes[k].id) +
												" has unresolved operands (undefined or cyclic ref)");
			}
			return true;
		}

		B32 Parser::wireDeferredInputs(const List<ParsedNode>& nodes) {
			for(const ParsedNode& pn : nodes) {
				if(pn.op != Opcode::Region && pn.op != Opcode::Phi && pn.op != Opcode::Stop)
					continue;
				Node* n = byId[pn.id];
				for(U32 i = 0; i < pn.operands.size(); ++i) {
					Node* in = operand(pn, i);
					if(!in)
						return false;
					n->addInput(in);
				}
			}
			return true;
		}

		B32 Parser::build(Function* fn, const List<ParsedNode>& nodes) {
			seedStartStop(fn, nodes);
			return materialize(fn, nodes) && wireDeferredInputs(nodes);
		}
	} // namespace detail

	B32 parseText(std::istream& in, Module& module, std::ostream& errors) {
		return detail::Parser(module, errors).parse(in);
	}

	B32 parseText(const String& text, Module& module, std::ostream& errors) {
		std::istringstream ss(text);
		return parseText(ss, module, errors);
	}
} // namespace rat
