#ifndef RAT_IR_TEXTPARSER_H
#define RAT_IR_TEXTPARSER_H

#include "Core.h"
#include "IR/Opcode.h"

namespace rat {
	struct Module;
	struct Type;
	struct Function;
	struct Node;

	namespace detail {
		B32 allDigits(const String& s);
		List<U32> parseVRefs(const String& s);
		B32 unquoteBytes(const String& s, List<U8>& out);
		Opcode opcodeForMnemonic(const String& m, B32& ok);

		struct ParsedNode {
			U32 id = 0;
			Opcode op = Opcode::Start;
			Type* ty = nullptr;
			I64 cval = 0;							 // Constant
			U32 projIndex = 0;				 // Proj
			String projLabel;					 // Proj
			String callee;						 // Call
			String symbol;						 // Global
			Type* allocType = nullptr; // Alloc
			B32 loopHeader = false;		 // Region
			List<U32> operands;
		};

		struct Parser {
			Module& mod;
			std::ostream& err;
			U32 lineNo = 0;
			B32 failed = false;

			Parser(Module& mod, std::ostream& err);

			B32 fail(const String& msg);
			B32 skip(const String& t);
			B32 parse(std::istream& in);
			Type* parseType(const String& s);
			B32 parseGlobal(const String& line);
			B32 parseFunction(const String& header, std::istream& in);
			B32 parseNodeLine(const String& line, ParsedNode& pn);

			B32 build(Function* fn, const List<ParsedNode>& nodes);
			void seedStartStop(Function* fn, const List<ParsedNode>& nodes);
			B32 materialize(Function* fn, const List<ParsedNode>& nodes);
			Node* makeNode(Function* fn, const ParsedNode& pn);
			B32 wireDeferredInputs(const List<ParsedNode>& nodes);

			Node* operand(const ParsedNode& pn, U32 index);

			Map<U32, Node*> byId;			 // parsed id -> materialized node
			Node* startCtrl = nullptr; // start control projection, if present
			Node* startMem = nullptr;	 // Start memory projection, if present
		};
	} // namespace detail

	B32 parseText(std::istream& in, Module& module, std::ostream& errors);
	B32 parseText(const String& text, Module& module, std::ostream& errors);
} // namespace rat

#endif
