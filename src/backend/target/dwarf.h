// lowers what the x86-64 encoder recorded per function to .eh_frame
#ifndef RAT_TARGET_DWARF_H
#define RAT_TARGET_DWARF_H

#include "core.h"

#include "target/object_file.h"

namespace rat {
	struct Function;

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

	struct EncodedFunc {
		const Function* fn;
		U32 textOff;
		U32 size;
		List<CfiOp> cfi;
	};

	// .eh_frame for the encoded functions
	void writeDwarf(const List<EncodedFunc>& funcs, ObjectFile& obj);

	namespace detail {
		void cfiOp(List<U8>& out, const CfiOp& op);
		void closeRecord(List<U8>& out, U32 start);
		void writeEhFrame(const List<EncodedFunc>& funcs, ObjectFile& obj);
	} // namespace detail
} // namespace rat

#endif
