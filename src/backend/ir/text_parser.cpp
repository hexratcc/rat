#include "ir/text_parser.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "ir/function.h"
#include "ir/module.h"
#include "ir/node.h"
#include "ir/type.h"
#include "string.h"

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

		void splitTypeToken(const String& after, String& typeStr, String& remainder) {
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
				return;
			}
			if(!after.empty() && after.front() == '<') {
				U64 close = after.find('>');
				U32 i = close == String::npos ? (U32)after.size() : (U32)close + 1;
				typeStr = after.substr(0, i);
				remainder = after.substr(i);
				return;
			}
			U64 sp = after.find(' ');
			typeStr = after.substr(0, sp);
			remainder = sp == String::npos ? "" : after.substr(sp);
		}

		B32 takeQuoted(const String& s, String& name, String& rest) {
			U64 q1 = s.find('"');
			U64 q2 = (q1 == String::npos) ? String::npos : s.find('"', q1 + 1);
			if(q2 == String::npos)
				return false;
			name = s.substr(q1 + 1, q2 - q1 - 1);
			rest = s.substr(q2 + 1);
			return true;
		}

		B32 hexDigit(C8 c, U8& v) {
			if(c >= '0' && c <= '9')
				v = (U8)(c - '0');
			else if(c >= 'a' && c <= 'f')
				v = (U8)(c - 'a' + 10);
			else if(c >= 'A' && c <= 'F')
				v = (U8)(c - 'A' + 10);
			else
				return false;
			return true;
		}

		B32 decodeBytes(const String& s, List<U8>& out) {
			for(U32 i = 0; i < s.size();) {
				if(s[i] == '\\') {
					U8 hi;
					U8 lo;
					if(i + 2 >= s.size() || !hexDigit(s[i + 1], hi) || !hexDigit(s[i + 2], lo))
						return false;
					out.push_back((U8)((hi << 4) | lo));
					i += 3;
				} else {
					out.push_back((U8)s[i]);
					++i;
				}
			}
			return true;
		}

		B32 unquoteBytes(const String& s, List<U8>& out) {
			String t = trim(s);
			if(t.size() < 2 || t.front() != '"' || t.back() != '"')
				return false;
			return decodeBytes(t.substr(1, t.size() - 2), out);
		}

		B32 parseIndex(const String& digits, U32& out) {
			errno = 0;
			U64 v = std::strtoul(digits.c_str(), nullptr, 10);
			out = (U32)v;
			return errno != ERANGE && v <= 0xffffffffUL;
		}

		Opcode opcodeForMnemonic(const String& m, B32& ok) {
			for(U32 i = (U32)Opcode::Start; i <= (U32)Opcode::Select; ++i) {
				if(m == getOpcodeMnemonic((Opcode)i)) {
					ok = true;
					return (Opcode)i;
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
			return true;
		}

		std::nullptr_t Parser::failNull(const String& msg) {
			fail(msg);
			return nullptr;
		}

		Type* Parser::parseType(const String& s) {
			String t = trim(s);
			if(t.empty())
				return failNull("empty type");
			if(t.front() == '(') {
				if(t.back() != ')')
					return failNull("unbalanced tuple type: " + t);
				List<Type*> elems;
				if(!parseTypeList(t.substr(1, t.size() - 2), elems))
					return nullptr;
				return mod.getTuple(elems);
			}
			U32 count = 0;
			if(t.front() == '[') {
				Type* elem = parseCounted(t, "array", "array count", count);
				return elem ? mod.getArray(elem, count) : nullptr;
			}
			if(t.front() == '<') {
				Type* elem = parseCounted(t, "vector", "vector lane count", count);
				return elem ? mod.getVec(elem, count) : nullptr;
			}
			if(t == "ctrl")
				return mod.getControl();
			if(t == "mem")
				return mod.getMemory();
			if(t == "ptr")
				return mod.getPtr();
			if(t.size() >= 2 && t[0] == 'i' && allDigits(t.substr(1)))
				return mod.getInt((U32)std::stoul(t.substr(1)));
			if(t.size() >= 2 && t[0] == 'f' && allDigits(t.substr(1)))
				return mod.getFloat((U32)std::stoul(t.substr(1)));
			return failNull("unknown type '" + t + "'");
		}

		Type* Parser::parseCounted(const String& t, const C8* kind, const C8* countName, U32& count) {
			C8 close = t.front() == '[' ? ']' : '>';
			if(t.back() != close)
				return failNull("unbalanced " + String(kind) + " type: " + t);
			String inner = t.substr(1, t.size() - 2);
			U64 x = inner.find(" x ");
			if(x == String::npos)
				return failNull(String(kind) + " type must be '" + t.front() + "N x T" + close + "': " + t);
			String countStr = trim(inner.substr(0, x));
			if(!allDigits(countStr))
				return failNull("bad " + String(countName) + " in: " + t);
			Type* elem = parseType(inner.substr(x + 3));
			if(elem)
				count = (U32)std::stoul(countStr);
			return elem;
		}

		B32 Parser::parseTypeList(const String& s, List<Type*>& out) {
			U32 depth = 0;
			String cur;
			for(C8 c : s) {
				if(c == '(')
					++depth;
				if(c == ')')
					--depth;
				if(c == ',' && depth == 0) {
					if(!flushType(cur, out))
						return false;
				} else {
					cur.push_back(c);
				}
			}
			return flushType(cur, out);
		}

		B32 Parser::flushType(String& cur, List<Type*>& out) {
			String e = trim(cur);
			cur.clear();
			if(e.empty())
				return true;
			Type* et = parseType(e);
			if(!et)
				return false;
			out.push_back(et);
			return true;
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

			B32 noInline = false;
			if(retStr.size() > 9 && retStr.substr(retStr.size() - 9) == " noinline") {
				noInline = true;
				retStr = trim(retStr.substr(0, retStr.size() - 9));
			}

			List<Type*> params;
			if(!parseTypeList(paramsStr, params))
				return false;

			Type* ret = nullptr;
			if(retStr != "void") {
				ret = parseType(retStr);
				if(!ret)
					return false;
			}

			Function* fn = mod.createFunction(name, params, ret);
			fn->getAttrs().noInline = noInline;

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
			if(!parseIndex(lhs.substr(1), pn.id))
				return fail("result id out of range '" + lhs + "'");

			String rest = line.substr(eq + 3);
			U64 colon = rest.find(" : ");
			if(colon == String::npos)
				return fail("expected ' : ' in: " + line);
			String mnem = trim(rest.substr(0, colon));
			B32 ok = false;
			pn.op = opcodeForMnemonic(mnem, ok);
			if(!ok)
				return fail("unknown mnemonic '" + mnem + "'");

			String typeStr;
			String remainder;
			splitTypeToken(ltrim(rest.substr(colon + 3)), typeStr, remainder);
			pn.ty = parseType(typeStr);
			if(!pn.ty)
				return false;
			return parsePayload(trim(remainder), line, pn);
		}

		B32 Parser::parsePayload(const String& remainder, const String& line, ParsedNode& pn) {
			switch(pn.op) {
			case Opcode::Constant: {
				if(remainder.empty())
					return fail("constant is missing its value: " + line);
				errno = 0;
				C8* cend = nullptr;
				pn.cval = (I64)std::strtoll(remainder.c_str(), &cend, 10);
				if(cend == remainder.c_str() || errno == ERANGE)
					return fail("bad constant value '" + remainder + "'");
				return true;
			}
			case Opcode::Proj:
				return parseProj(remainder, line, pn);
			case Opcode::Call:
			case Opcode::Global: {
				String name;
				String rest;
				if(!takeQuoted(remainder, name, rest))
					return fail("node is missing its quoted name: " + line);
				if(pn.op == Opcode::Call) {
					pn.callee = std::move(name);
					pn.operands = parseVRefs(rest);
				} else {
					pn.symbol = std::move(name);
				}
				return true;
			}
			case Opcode::Asm: {
				String text;
				String rest;
				if(!takeQuoted(remainder, text, rest))
					return fail("asm node is missing its quoted template: " + line);
				List<U8> bytes;
				if(!decodeBytes(text, bytes))
					return fail("malformed asm template: " + line);
				pn.asmText.assign(bytes.begin(), bytes.end());
				pn.operands = parseVRefs(rest);
				return true;
			}
			case Opcode::Alloc:
				if(remainder.empty())
					return fail("alloc node is missing its type: " + line);
				pn.allocType = parseType(remainder);
				return pn.allocType != nullptr;
			case Opcode::Region: {
				String body = remainder; // already trimmed
				pn.loopHeader = takeKeyword(body, "loop");
				pn.operands = parseVRefs(body);
				return true;
			}
			case Opcode::Load:
			case Opcode::Store: {
				String body = remainder;
				pn.isVolatile = takeKeyword(body, "volatile");
				pn.operands = parseVRefs(body);
				return true;
			}
			case Opcode::Extract:
			case Opcode::Shuffle:
				return parseLaneOp(remainder, line, pn);
			default:
				pn.operands = parseVRefs(remainder);
				return true;
			}
		}

		// a leading keyword followed by a space or the end
		B32 Parser::takeKeyword(String& body, const C8* word) {
			U32 n = (U32)std::strlen(word);
			if(body.rfind(word, 0) != 0 || (body.size() != n && !std::isspace((U8)body[n])))
				return false;
			body = body.substr(n);
			return true;
		}

		B32 Parser::parseProj(const String& remainder, const String& line, ParsedNode& pn) {
			std::istringstream ss(remainder);
			String index;
			String label;
			ss >> index >> label;
			if(index.empty() || index[0] != '#')
				return fail("malformed proj (expected #index): " + line);
			if(!allDigits(index.substr(1)))
				return fail("bad proj index '" + index + "'");
			if(!parseIndex(index.substr(1), pn.projIndex))
				return fail("proj index out of range '" + index + "'");
			if(label.size() >= 2 && label.front() == '"' && label.back() == '"')
				pn.projLabel = label.substr(1, label.size() - 2);
			return singleRef(remainder, "proj must reference exactly one producer: " + line, pn);
		}

		B32 Parser::parseLaneOp(const String& remainder, const String& line, ParsedNode& pn) {
			String laneTok = remainder.substr(0, remainder.find(' '));
			if(laneTok.size() < 2 || laneTok[0] != '#' || !allDigits(laneTok.substr(1)))
				return fail("malformed lane selector (expected #index): " + line);
			// reuse the proj payload slot for the selector
			if(!parseIndex(laneTok.substr(1), pn.projIndex))
				return fail("lane selector out of range: " + line);
			return singleRef(remainder, "lane op must reference exactly one vector: " + line, pn);
		}

		B32 Parser::singleRef(const String& remainder, const String& error, ParsedNode& pn) {
			pn.operands = parseVRefs(remainder);
			return pn.operands.size() == 1 || fail(error);
		}

		Node* Parser::operand(const ParsedNode& pn, U32 index) {
			if(index >= pn.operands.size())
				return failNull("v" + std::to_string(pn.id) + " (" + getOpcodeMnemonic(pn.op) +
												") is missing operand " + std::to_string(index));
			auto it = byId.find(pn.operands[index]);
			if(it == byId.end())
				return failNull("v" + std::to_string(pn.id) + " references undefined v" +
												std::to_string(pn.operands[index]));
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

		// fetches the first `count` operands of `pn`; false on a missing or undefined one
		B32 Parser::operands(const ParsedNode& pn, U32 count, List<Node*>& out) {
			for(U32 i = 0; i < count; ++i) {
				Node* n = operand(pn, i);
				if(!n)
					return false;
				out.push_back(n);
			}
			return true;
		}

		// constructs the single node described by `pn`, looking up its operands via
		// operands(). Region / Phi are created with no inputs (wired later by
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
			if(op == Opcode::Global)
				return fn->create<GlobalNode>(pn.ty, pn.symbol);
			if(op == Opcode::Alloc)
				return fn->create<AllocNode>(pn.ty, pn.allocType);

			// fixed-arity ops take the table count (extra refs are ignored), variadic ones all refs
			I8 maxInputs = getOpcodeInfo(op).maxInputs;
			U32 count = (U32)maxInputs;
			if(maxInputs < 0 || op == Opcode::Return)
				count = (U32)pn.operands.size();
			List<Node*> in;
			if(!operands(pn, count, in))
				return nullptr;
			return makeWithInputs(fn, pn, in);
		}

		Node* Parser::makeWithInputs(Function* fn, const ParsedNode& pn, const List<Node*>& in) {
			Opcode op = pn.op;
			switch(op) {
			case Opcode::If:
				return fn->create<IfNode>(pn.ty, in[0], in[1]);
			case Opcode::Switch:
				return fn->create<SwitchNode>(pn.ty, in[0], in[1]);
			case Opcode::Proj:
				if(in[0] == fn->getStart() && pn.projIndex == StartNode::controlProjIndex() && startCtrl)
					return startCtrl;
				if(in[0] == fn->getStart() && pn.projIndex == StartNode::memoryProjIndex() && startMem)
					return startMem;
				return fn->create<ProjNode>(pn.ty, in[0], pn.projIndex, pn.projLabel);
			case Opcode::Load: {
				LoadNode* l = fn->create<LoadNode>(pn.ty, in[0], in[1], in[2]);
				l->setVolatile(pn.isVolatile);
				return l;
			}
			case Opcode::Store: {
				StoreNode* s = fn->create<StoreNode>(pn.ty, in[0], in[1], in[2], in[3]);
				s->setVolatile(pn.isVolatile);
				return s;
			}
			case Opcode::Return:
				return fn->create<ReturnNode>(pn.ty, in);
			case Opcode::Call: {
				B32 rv = pn.ty->isTuple() && pn.ty->getTupleElementCount() == 3;
				return fn->create<CallNode>(pn.ty, pn.callee, rv, in);
			}
			case Opcode::Asm: {
				U32 outs = pn.ty->isTuple() ? pn.ty->getTupleElementCount() - 2 : 0;
				return fn->create<AsmNode>(pn.ty, pn.asmText, outs, in);
			}
			case Opcode::StackSave:
				return fn->create<StackSaveNode>(pn.ty, in[0], in[1]);
			case Opcode::StackAlloc:
				return fn->create<StackAllocNode>(pn.ty, in[0], in[1], in[2]);
			case Opcode::StackRestore:
				return fn->create<StackRestoreNode>(pn.ty, in[0], in[1], in[2]);
			case Opcode::Splat:
				return fn->create<SplatNode>(pn.ty, in[0]);
			case Opcode::Extract:
				return fn->create<ExtractNode>(pn.ty, in[0], pn.projIndex);
			case Opcode::Shuffle:
				return fn->create<ShuffleNode>(pn.ty, in[0], (U8)pn.projIndex);
			case Opcode::Select:
				return fn->create<SelectNode>(pn.ty, in[0], in[1], in[2]);
			case Opcode::Pack:
				return fn->create<PackNode>(pn.ty, in);
			default:
				break;
			}
			switch(getOpClass(op)) {
			case OpClass::Binary:
				return fn->create<BinaryNode>(op, pn.ty, in[0], in[1]);
			case OpClass::Compare:
				return fn->create<CompareNode>(op, pn.ty, in[0], in[1]);
			case OpClass::Unary:
				return fn->create<UnaryNode>(op, pn.ty, in[0]);
			case OpClass::Convert:
				return fn->create<ConvertNode>(op, pn.ty, in[0]);
			case OpClass::None:
				break;
			}
			return failNull(String("cannot construct opcode '") + getOpcodeMnemonic(op) + "'");
		}

		// a record is ready once every operand it references already exists;
		// Region / Phi are exempt since their inputs are wired in a later pass
		B32 Parser::ready(const ParsedNode& pn) const {
			if(pn.op == Opcode::Region || pn.op == Opcode::Phi)
				return true;
			for(U32 v : pn.operands)
				if(!byId.count(v))
					return false;
			return true;
		}

		B32 Parser::materialize(Function* fn, const List<ParsedNode>& nodes) {
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

			for(U32 k = 0; k < nodes.size(); ++k)
				if(!done[k])
					return fail("v" + std::to_string(nodes[k].id) +
											" has unresolved operands (undefined or cyclic ref)");
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

	B32 parseText(const String& text, Module& module, std::ostream& errors) {
		std::istringstream ss(text);
		return detail::Parser(module, errors).parse(ss);
	}
} // namespace rat
