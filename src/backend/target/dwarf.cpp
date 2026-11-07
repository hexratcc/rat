#include "target/dwarf.h"

#include "byte_io.h"

namespace rat {
	namespace detail {
		constexpr U8 DW_CFA_advance_loc = 0x40;
		constexpr U8 DW_CFA_offset = 0x80;
		constexpr U8 DW_CFA_advance_loc4 = 0x04;
		constexpr U8 DW_CFA_def_cfa = 0x0c;

		constexpr U8 DW_EH_PE_pcrel_sdata4 = 0x1b;

		// x86-64, rsp and the return address column
		constexpr U8 kPtrBytes = 8;
		constexpr U32 kDwarfSp = 7;
		constexpr U32 kDwarfRa = 16;

		void cfiOp(List<U8>& out, const CfiOp& op) {
			if(op.kind == CfiOp::Kind::DefCfa) {
				out.insert(out.end(), {DW_CFA_def_cfa, (U8)op.reg});
				return le::uleb(out, (U64)op.value);
			}
			assert(op.reg < 0x40 && op.value < 0 && "saved register out of DW_CFA_offset range");
			out.push_back(DW_CFA_offset | (U8)op.reg);
			le::uleb(out, (U64)(-op.value / kPtrBytes)); // factored by the cie data alignment
		}

		// pads a record with DW_CFA_nop to the pointer size and patches its length
		void closeRecord(List<U8>& out, U32 start) {
			le::padTo(out, start + (out.size() - start + kPtrBytes - 1) / kPtrBytes * kPtrBytes);
			le::patch32(out, start, (U32)(out.size() - start - 4));
		}

		void writeEhFrame(const List<EncodedFunc>& funcs, ObjectFile& obj) {
			List<U8> out;
			le::put64(out, 0); // length, cie id
			// version, "zR", code alignment, data alignment -8 as sleb, return address column, pointer encoding
			out.insert(out.end(), {1, 'z', 'R', 0, 1, 0x78, kDwarfRa, 1, DW_EH_PE_pcrel_sdata4});
			// on entry the cfa is just above the return address
			cfiOp(out, {0, CfiOp::Kind::DefCfa, kDwarfSp, kPtrBytes});
			cfiOp(out, {0, CfiOp::Kind::Offset, kDwarfRa, -kPtrBytes});
			closeRecord(out, 0);
			U32 base = obj.sectionSize(ObjectFile::EhFrame);
			String textSym = obj.sectionSymbol(ObjectFile::Text);
			for(const EncodedFunc& func : funcs) {
				U32 start = (U32)out.size();
				le::put32(out, 0);
				le::put32(out, start + 4); // back to the cie at 0
				obj.addReloc(
						ObjectFile::EhFrame, base + (U32)out.size(), textSym, RelocKind::Pc32, func.textOff);
				le::put32(out, 0);
				le::put32(out, func.size);
				out.push_back(0); // augmentation data length
				U32 at = 0;
				for(const CfiOp& op : func.cfi) {
					U32 delta = op.at - at;
					if(delta >= 0x40) {
						out.push_back(DW_CFA_advance_loc4);
						le::put32(out, delta);
					} else if(delta) {
						out.push_back(DW_CFA_advance_loc | (U8)delta);
					}
					at = op.at;
					cfiOp(out, op);
				}
				closeRecord(out, start);
			}
			obj.append(ObjectFile::EhFrame, out.data(), (U32)out.size());
		}
	} // namespace detail

	void writeDwarf(const List<EncodedFunc>& funcs, ObjectFile& obj) {
		if(!funcs.empty())
			detail::writeEhFrame(funcs, obj);
	}
} // namespace rat
