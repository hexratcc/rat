// lowers DebugInfo plus what the x86-64 encoder recorded per function to DWARF 5 and .eh_frame
#ifndef RAT_TARGET_DWARF_H
#define RAT_TARGET_DWARF_H

#include "core.h"

#include "ir/debug_info.h"
#include "target/x86/x86_asm.h"

namespace rat {
	struct Function;
	struct Module;

	// a call frame rule change at a code offset, regs are dwarf numbers
	struct CfiOp {
		enum class Kind : U8 {
			DefCfa, // cfa = reg + value
			Offset	// reg saved at cfa + value
		};
		U32 at;
		Kind kind;
		U32 reg;
		I32 value;
	};

	struct LineRow {
		U32 at;
		SrcLoc loc;
	};

	struct PcRange {
		U32 start;
		U32 end;
	};

	struct EncodedFunc {
		const Function* fn;
		U32 textOff;
		U32 size;
		List<CfiOp> cfi;
		List<LineRow> lines;
	};

	// .eh_frame always, the .debug_* sections when the module carries debug info
	void writeDwarf(const Module& mod, const List<EncodedFunc>& funcs, ObjectFile& obj);

	namespace detail {
		struct Die {
			List<U8> decl; // attr and form pairs
			List<U8> body;
			List<AsmReloc> relocs; // offsets in body

			void attr(U32 name, U32 form);
			void string(U32 name, const String& s);
			void udata(U32 name, U64 v);
			// an addr or a sec_offset
			void reloc(U32 name, U32 form, const String& symbol, I64 addend);
			void exprloc(U32 name, const List<U8>& expr);
			void srcLoc(U32 fileAttr, SrcLoc loc);
		};

		void describeFunc(Die& d, const String& name, const DebugFunc& f, B32 external);

		struct DieWriter {
			DieWriter(const Module& m, ObjectFile& o)
			: mod(m),
				obj(o) {}

			const Module& mod;
			ObjectFile& obj;
			List<U8> info;
			List<U8> abbrev;
			Map<String, U32> abbrevCode; // encoded declaration -> code

			U32 emit(U32 tag, B32 children, const Die& d);
			void pcRanges(Die& d, const List<PcRange>& r, U32 textOff);
			void endChildren(B32 variadic, B32 children);
			void funcDie(const EncodedFunc& func);
			void unit(const List<EncodedFunc>& funcs);
		};

		void unitHeader(List<U8>& out);
		void writeLineSequence(List<U8>& out, U32 at, const EncodedFunc& func, ObjectFile& obj);
		void writeLineTable(const DebugInfo& di, const List<EncodedFunc>& funcs, ObjectFile& obj);
		void cfiOp(List<U8>& out, const CfiOp& op);
		void closeRecord(List<U8>& out, U32 start);
		void writeEhFrame(const List<EncodedFunc>& funcs, ObjectFile& obj);
	} // namespace detail
} // namespace rat

#endif
