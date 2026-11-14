#include "target/dwarf.h"

#include "byte_io.h"
#include "ir/module.h"

namespace rat {
	namespace detail {
		constexpr U32 DW_TAG_array_type = 0x01;
		constexpr U32 DW_TAG_enumeration_type = 0x04;
		constexpr U32 DW_TAG_formal_parameter = 0x05;
		constexpr U32 DW_TAG_lexical_block = 0x0b;
		constexpr U32 DW_TAG_member = 0x0d;
		constexpr U32 DW_TAG_pointer_type = 0x0f;
		constexpr U32 DW_TAG_compile_unit = 0x11;
		constexpr U32 DW_TAG_structure_type = 0x13;
		constexpr U32 DW_TAG_subroutine_type = 0x15;
		constexpr U32 DW_TAG_typedef = 0x16;
		constexpr U32 DW_TAG_union_type = 0x17;
		constexpr U32 DW_TAG_unspecified_parameters = 0x18;
		constexpr U32 DW_TAG_inlined_subroutine = 0x1d;
		constexpr U32 DW_TAG_subrange_type = 0x21;
		constexpr U32 DW_TAG_base_type = 0x24;
		constexpr U32 DW_TAG_const_type = 0x26;
		constexpr U32 DW_TAG_enumerator = 0x28;
		constexpr U32 DW_TAG_subprogram = 0x2e;
		constexpr U32 DW_TAG_variable = 0x34;
		constexpr U32 DW_TAG_volatile_type = 0x35;

		constexpr U32 DW_AT_location = 0x02;
		constexpr U32 DW_AT_name = 0x03;
		constexpr U32 DW_AT_byte_size = 0x0b;
		constexpr U32 DW_AT_bit_size = 0x0d;
		constexpr U32 DW_AT_stmt_list = 0x10;
		constexpr U32 DW_AT_low_pc = 0x11;
		constexpr U32 DW_AT_high_pc = 0x12;
		constexpr U32 DW_AT_language = 0x13;
		constexpr U32 DW_AT_comp_dir = 0x1b;
		constexpr U32 DW_AT_const_value = 0x1c;
		constexpr U32 DW_AT_inline = 0x20;
		constexpr U32 DW_AT_producer = 0x25;
		constexpr U32 DW_AT_prototyped = 0x27;
		constexpr U32 DW_AT_abstract_origin = 0x31;
		constexpr U32 DW_AT_count = 0x37;
		constexpr U32 DW_AT_data_member_location = 0x38;
		constexpr U32 DW_AT_decl_file = 0x3a;
		constexpr U32 DW_AT_declaration = 0x3c;
		constexpr U32 DW_AT_encoding = 0x3e;
		constexpr U32 DW_AT_external = 0x3f;
		constexpr U32 DW_AT_frame_base = 0x40;
		constexpr U32 DW_AT_type = 0x49;
		constexpr U32 DW_AT_ranges = 0x55;
		constexpr U32 DW_AT_call_file = 0x58;
		constexpr U32 DW_AT_data_bit_offset = 0x6b;

		constexpr U32 DW_FORM_addr = 0x01;
		constexpr U32 DW_FORM_string = 0x08;
		constexpr U32 DW_FORM_sdata = 0x0d;
		constexpr U32 DW_FORM_udata = 0x0f;
		constexpr U32 DW_FORM_ref4 = 0x13;
		constexpr U32 DW_FORM_sec_offset = 0x17;
		constexpr U32 DW_FORM_exprloc = 0x18;
		constexpr U32 DW_FORM_flag_present = 0x19;

		constexpr U8 DW_OP_addr = 0x03;
		constexpr U8 DW_OP_deref = 0x06;
		constexpr U8 DW_OP_consts = 0x11;
		constexpr U8 DW_OP_regx = 0x90;
		constexpr U8 DW_OP_fbreg = 0x91;
		constexpr U8 DW_OP_bregx = 0x92;
		constexpr U8 DW_OP_call_frame_cfa = 0x9c;
		constexpr U8 DW_OP_stack_value = 0x9f;

		constexpr U8 DW_LE_end_of_list = 0;
		constexpr U8 DW_LE_offset_pair = 4; // same in rnglists and loclists

		constexpr U8 DW_LNS_copy = 1;
		constexpr U8 DW_LNS_advance_pc = 2;
		constexpr U8 DW_LNS_advance_line = 3;
		constexpr U8 DW_LNS_set_file = 4;
		constexpr U8 DW_LNS_set_column = 5;

		constexpr U8 DW_LNE_end_sequence = 1;
		constexpr U8 DW_LNE_set_address = 2;

		constexpr U8 DW_LNCT_path = 1;
		constexpr U8 DW_LNCT_directory_index = 2;

		constexpr U8 DW_CFA_advance_loc = 0x40;
		constexpr U8 DW_CFA_offset = 0x80;
		constexpr U8 DW_CFA_advance_loc4 = 0x04;
		constexpr U8 DW_CFA_def_cfa = 0x0c;

		constexpr U8 DW_EH_PE_pcrel_sdata4 = 0x1b;
		constexpr U8 DW_LANG_C99 = 0x0c;
		constexpr U8 DW_UT_compile = 1;
		constexpr U8 DW_INL_inlined = 1;

		constexpr U8 DW_ATE_boolean = 0x02;
		constexpr U8 DW_ATE_complex_float = 0x03;
		constexpr U8 DW_ATE_float = 0x04;
		constexpr U8 DW_ATE_signed = 0x05;
		constexpr U8 DW_ATE_signed_char = 0x06;
		constexpr U8 DW_ATE_unsigned = 0x07;
		constexpr U8 DW_ATE_unsigned_char = 0x08;

		// per base DebugType kind
		constexpr U8 kBaseEncoding[] = {
				DW_ATE_signed,
				DW_ATE_unsigned,
				DW_ATE_signed_char,
				DW_ATE_unsigned_char,
				DW_ATE_boolean,
				DW_ATE_float,
				DW_ATE_complex_float,
		};

		// die tag per DebugType kind from Pointer on
		constexpr U32 kTypeTag[] = {
				DW_TAG_pointer_type,
				DW_TAG_structure_type,
				DW_TAG_union_type,
				DW_TAG_array_type,
				DW_TAG_subroutine_type,
				DW_TAG_typedef,
				DW_TAG_enumeration_type,
				DW_TAG_const_type,
				DW_TAG_volatile_type,
		};

		// x86-64, rsp and the return address column
		constexpr U8 kPtrBytes = 8;
		constexpr U32 kDwarfSp = 7;
		constexpr U32 kDwarfRa = 16;
	} // namespace detail

	B32 VarLoc::operator==(const VarLoc& o) const {
		return kind == o.kind && reg == o.reg && value == o.value;
	}

	namespace detail {
		void Die::attr(U32 name, U32 form) {
			le::uleb(decl, name);
			le::uleb(decl, form);
		}

		void Die::string(U32 name, const String& s) {
			attr(name, DW_FORM_string);
			le::putString(body, s);
		}

		void Die::udata(U32 name, U64 v) {
			attr(name, DW_FORM_udata);
			le::uleb(body, v);
		}

		void Die::reloc(U32 name, U32 form, const String& symbol, I64 addend) {
			attr(name, form);
			if(form == DW_FORM_exprloc) // the address of a static
				body.insert(body.end(), {1 + kPtrBytes, DW_OP_addr});
			RelocKind kind = form == DW_FORM_sec_offset ? RelocKind::Abs32 : RelocKind::Abs64;
			relocs.push_back({(U32)body.size(), symbol, kind, addend});
			body.insert(body.end(), kind == RelocKind::Abs64 ? 8 : 4, 0);
		}

		void Die::exprloc(U32 name, const List<U8>& expr) {
			attr(name, DW_FORM_exprloc);
			le::uleb(body, expr.size());
			body.insert(body.end(), expr.begin(), expr.end());
		}

		void Die::ref(U32 name, const void* target) {
			attr(name, DW_FORM_ref4);
			refs.push_back({(U32)body.size(), target});
			le::put32(body, 0);
		}

		void Die::type(U32 name, const DebugType* t) {
			ref(name, t);
			types.push_back(t);
		}

		// line follows the file attr and column precedes it, for decl_* and call_* alike
		void Die::srcLoc(U32 fileAttr, SrcLoc loc) {
			udata(fileAttr, loc.file);
			udata(fileAttr + 1, loc.line);
			if(loc.col)
				udata(fileAttr - 1, loc.col);
		}

		void describeVar(Die& d, const DebugVar& v) {
			d.string(DW_AT_name, v.name);
			d.srcLoc(DW_AT_decl_file, v.decl);
			d.type(DW_AT_type, v.type);
		}

		void describeFunc(Die& d, const String& name, const DebugFunc& f, B32 external) {
			d.string(DW_AT_name, name);
			d.srcLoc(DW_AT_decl_file, f.decl);
			d.attr(DW_AT_prototyped, DW_FORM_flag_present);
			if(f.ret)
				d.type(DW_AT_type, f.ret);
			if(external)
				d.attr(DW_AT_external, DW_FORM_flag_present);
		}

		List<U8> locExpr(const VarLoc& loc, B32 indirect) {
			List<U8> out;
			if(loc.kind == VarLoc::Kind::Const) { // a constant address names memory
				out.push_back(DW_OP_consts);
				le::sleb(out, loc.value);
				if(!indirect)
					out.push_back(DW_OP_stack_value);
			} else if(loc.kind == VarLoc::Kind::Frame) {
				out.push_back(DW_OP_fbreg);
				le::sleb(out, loc.value);
				if(indirect)
					out.push_back(DW_OP_deref);
			} else if(indirect) { // a register holding the address names the memory it points at
				out.insert(out.end(), {DW_OP_bregx, (U8)loc.reg, 0});
			} else {
				out.insert(out.end(), {DW_OP_regx, (U8)loc.reg});
			}
			return out;
		}

		// offsets are from the unit base, .text
		void offsetPair(List<U8>& out, U32 textOff, U32 start, U32 end) {
			out.push_back(DW_LE_offset_pair);
			le::uleb(out, textOff + start);
			le::uleb(out, textOff + end);
		}

		// appends d to .debug_info under its abbreviation code, declaring it once. a die that may have
		// children always declares them, its list may end right away
		U32 DieWriter::emit(U32 tag, B32 children, const Die& d) {
			List<U8> key;
			le::uleb(key, tag);
			key.push_back(children ? 1 : 0);
			key.insert(key.end(), d.decl.begin(), d.decl.end());
			key.insert(key.end(), {0, 0});
			auto [it, fresh] = abbrevCode.emplace(String(key.begin(), key.end()), 0);
			if(fresh) {
				it->second = (U32)abbrevCode.size();
				le::uleb(abbrev, it->second);
				abbrev.insert(abbrev.end(), key.begin(), key.end());
			}
			U32 at = (U32)info.size();
			le::uleb(info, it->second);
			U32 bodyAt = (U32)info.size();
			for(const auto& [off, target] : d.refs)
				refs.push_back({bodyAt + off, target});
			for(const DebugType* t : d.types)
				if(dieOff.emplace(t, 0).second)
					pendingTypes.push_back(t);
			for(const AsmReloc& r : d.relocs)
				obj.addReloc(ObjectFile::DebugInfo, bodyAt + r.offset, r.symbol, r.kind, r.addend);
			info.insert(info.end(), d.body.begin(), d.body.end());
			return at;
		}

		// one range as low and high pc, several as a .debug_rnglists list. list offsets are from the
		// unit base, .text
		void DieWriter::pcRanges(Die& d, const List<PcRange>& r, U32 textOff) {
			if(r.size() == 1) {
				String textSym = obj.sectionSymbol(ObjectFile::Text);
				d.reloc(DW_AT_low_pc, DW_FORM_addr, textSym, textOff + r[0].start);
				return d.udata(DW_AT_high_pc, r[0].end - r[0].start);
			}
			String listSym = obj.sectionSymbol(ObjectFile::DebugRnglists);
			d.reloc(DW_AT_ranges, DW_FORM_sec_offset, listSym, (I64)rnglists.size());
			for(const PcRange& p : r)
				offsetPair(rnglists, textOff, p.start, p.end);
			rnglists.push_back(DW_LE_end_of_list);
		}

		void DieWriter::typeDie(const DebugType* t) {
			using Kind = DebugType::Kind;
			Kind k = t->kind;
			Die d;
			if(!t->name.empty())
				d.string(DW_AT_name, t->name);
			U32 tag = DW_TAG_base_type;
			if(k <= Kind::ComplexFloat)
				d.udata(DW_AT_encoding, kBaseEncoding[(U32)k]);
			else
				tag = kTypeTag[(U32)k - (U32)Kind::Pointer];
			if(t->incomplete)
				d.attr(DW_AT_declaration, DW_FORM_flag_present);
			else if(k <= Kind::Union || k == Kind::Enum)
				d.udata(DW_AT_byte_size, t->size);
			if(k == Kind::Function)
				d.attr(DW_AT_prototyped, DW_FORM_flag_present);
			if(k == Kind::Typedef || k == Kind::Enum)
				d.srcLoc(DW_AT_decl_file, t->decl);
			if(t->elem)
				d.type(DW_AT_type, t->elem);
			B32 hasChildLists = !t->members.empty() || !t->params.empty() || !t->enumerators.empty();
			B32 children = hasChildLists || k == Kind::Array || t->variadic;
			dieOff[t] = emit(tag, children, d);
			for(const DebugType::Member& m : t->members) {
				Die member;
				if(!m.name.empty())
					member.string(DW_AT_name, m.name);
				member.type(DW_AT_type, m.type);
				if(m.bitSize) {
					member.udata(DW_AT_data_bit_offset, (U64)m.offset * 8 + m.bitOffset);
					member.udata(DW_AT_bit_size, m.bitSize);
				} else {
					member.udata(DW_AT_data_member_location, m.offset);
				}
				emit(DW_TAG_member, false, member);
			}
			if(k == Kind::Array) {
				Die range;
				if(t->count)
					range.udata(DW_AT_count, t->count);
				emit(DW_TAG_subrange_type, false, range);
			}
			for(const DebugType* p : t->params) {
				Die param;
				param.type(DW_AT_type, p);
				emit(DW_TAG_formal_parameter, false, param);
			}
			for(const auto& [name, value] : t->enumerators) {
				Die e;
				e.string(DW_AT_name, name);
				e.attr(DW_AT_const_value, DW_FORM_sdata);
				le::sleb(e.body, value);
				emit(DW_TAG_enumerator, false, e);
			}
			endChildren(t->variadic, children);
		}

		// ends the children of a function or type
		void DieWriter::endChildren(B32 variadic, B32 children) {
			if(variadic)
				emit(DW_TAG_unspecified_parameters, false, {});
			if(children)
				info.push_back(0);
		}

		// a static whose storage was never emitted gets no die
		B32 DieWriter::hasStorage(const DebugVar& v) {
			return v.symbol.empty() || mod.getGlobal(v.symbol);
		}

		void DieWriter::globalDie(const DebugVar& v) {
			const Global* g = mod.getGlobal(v.symbol);
			if(!g)
				return;
			Die d;
			describeVar(d, v);
			if(!g->isInternal())
				d.attr(DW_AT_external, DW_FORM_flag_present);
			d.reloc(DW_AT_location, DW_FORM_exprloc, v.symbol, 0);
			emit(DW_TAG_variable, false, d);
		}

		// a variable copy, named by its abstract die when it sits in inlined code. without a location
		// gdb reports it as optimized out
		void DieWriter::instDie(const ScopeTree& tree, U32 inst) {
			const DebugVarInst& vi = tree.body.vars[inst];
			const DebugVar& v = *vi.var;
			if(!hasStorage(v))
				return;
			Die d;
			if(tree.inlinedIn[vi.scope])
				d.ref(DW_AT_abstract_origin, &v);
			else
				describeVar(d, v);
			I32 frameOff = tree.func.varFrameOff[inst];
			const List<VarRange>& r = tree.func.varRanges[inst];
			if(!v.symbol.empty()) {
				d.reloc(DW_AT_location, DW_FORM_exprloc, v.symbol, 0);
			} else if(frameOff != kNoFrameSlot) {
				VarLoc frameSlot{VarLoc::Kind::Frame, 0, frameOff};
				d.exprloc(DW_AT_location, locExpr(frameSlot, false));
			} else if(!r.empty()) {
				String listSym = obj.sectionSymbol(ObjectFile::DebugLoclists);
				d.reloc(DW_AT_location, DW_FORM_sec_offset, listSym, (I64)loclists.size());
				for(const VarRange& range : r) {
					offsetPair(loclists, tree.func.textOff, range.start, range.end);
					List<U8> expr = locExpr(range.loc, vi.indirect);
					le::uleb(loclists, expr.size());
					loclists.insert(loclists.end(), expr.begin(), expr.end());
				}
				loclists.push_back(DW_LE_end_of_list);
			}
			emit(v.isParam ? DW_TAG_formal_parameter : DW_TAG_variable, false, d);
		}

		void DieWriter::scopeDie(const ScopeTree& tree, U32 scope) {
			const DebugScope& s = tree.body.scopes[scope];
			Die d;
			if(s.inlined)
				d.ref(DW_AT_abstract_origin, s.inlined);
			pcRanges(d, tree.func.scopes[scope], tree.func.textOff);
			if(s.inlined)
				d.srcLoc(DW_AT_call_file, tree.body.locs[s.callLoc].src);
			B32 children = hasDies(tree, scope);
			emit(s.inlined ? DW_TAG_inlined_subroutine : DW_TAG_lexical_block, children, d);
			scopeChildren(tree, scope);
			endChildren(false, children);
		}

		// lexical blocks without code hand their vars and children to the parent, an inlined call
		// without code shows nothing
		void DieWriter::scopeChildren(const ScopeTree& tree, U32 scope) {
			for(U32 inst : tree.vars[scope])
				instDie(tree, inst);
			for(U32 kid : tree.kids[scope]) {
				B32 hasCode = !tree.func.scopes[kid].empty();
				if(tree.body.scopes[kid].inlined) {
					if(hasCode)
						scopeDie(tree, kid);
				} else if(hasCode && hasDies(tree, kid)) {
					scopeDie(tree, kid);
				} else {
					scopeChildren(tree, kid);
				}
			}
		}

		// whether scopeChildren emits anything
		B32 DieWriter::hasDies(const ScopeTree& tree, U32 scope) {
			for(U32 inst : tree.vars[scope])
				if(hasStorage(*tree.body.vars[inst].var))
					return true;
			for(U32 kid : tree.kids[scope])
				if(tree.body.scopes[kid].inlined ? !tree.func.scopes[kid].empty() : hasDies(tree, kid))
					return true;
			return false;
		}

		ScopeTree DieWriter::indexScopes(const EncodedFunc& func) {
			const DebugBody& body = *func.fn->getDebug();
			U32 n = (U32)body.scopes.size();
			ScopeTree tree{func, body, List<List<U32>>(n), List<List<U32>>(n), List<U32>(n, 0)};
			for(U32 s = 1; s < n; ++s) {
				const DebugScope& sc = body.scopes[s];
				tree.kids[sc.parent].push_back(s);
				tree.inlinedIn[s] = sc.inlined ? s : tree.inlinedIn[sc.parent]; // parents come first
				// every inlined func gets its abstract die
				if(sc.inlined && dieOff.emplace(sc.inlined, 0).second)
					pendingFuncs.push_back(sc.inlined);
			}
			for(U32 i = 0; i < body.vars.size(); ++i)
				tree.vars[body.vars[i].scope].push_back(i);
			return tree;
		}

		void DieWriter::funcDie(const EncodedFunc& func) {
			ScopeTree tree = indexScopes(func);
			Die d;
			describeFunc(d, func.fn->getName(), *tree.body.func, !func.fn->getAttrs().isInternal());
			pcRanges(d, {{0, func.size}}, func.textOff);
			d.exprloc(DW_AT_frame_base, {DW_OP_call_frame_cfa});
			B32 variadic = tree.body.func->variadic;
			B32 children = variadic || hasDies(tree, 0);
			emit(DW_TAG_subprogram, children, d);
			scopeChildren(tree, 0);
			endChildren(variadic, children);
		}

		// what inlined copies point at, names and types without code
		void DieWriter::abstractDie(const DebugFunc* f) {
			Die d;
			describeFunc(d, f->name, *f, f->external);
			d.udata(DW_AT_inline, DW_INL_inlined);
			B32 children = !f->vars.empty() || f->variadic;
			dieOff[f] = emit(DW_TAG_subprogram, children, d);
			for(const UniquePtr<DebugVar>& v : f->vars) {
				Die var;
				describeVar(var, *v);
				dieOff[v.get()] = emit(v->isParam ? DW_TAG_formal_parameter : DW_TAG_variable, false, var);
			}
			endChildren(f->variadic, children);
		}

		void DieWriter::unit(const List<EncodedFunc>& funcs) {
			const DebugInfo& di = *mod.getDebugInfo();
			le::put32(info, 0); // unit_length
			info.insert(info.end(), {5, 0, DW_UT_compile, kPtrBytes});
			String abbrevSym = obj.sectionSymbol(ObjectFile::DebugAbbrev);
			obj.addReloc(ObjectFile::DebugInfo, (U32)info.size(), abbrevSym, RelocKind::Abs32, 0);
			le::put32(info, 0);
			Die cu;
			cu.string(DW_AT_producer, di.producer);
			cu.udata(DW_AT_language, DW_LANG_C99);
			cu.string(DW_AT_name, di.files[0]);
			cu.string(DW_AT_comp_dir, di.compDir);
			pcRanges(cu, {{0, obj.sectionSize(ObjectFile::Text)}}, 0);
			cu.reloc(DW_AT_stmt_list, DW_FORM_sec_offset, obj.sectionSymbol(ObjectFile::DebugLine), 0);
			emit(DW_TAG_compile_unit, true, cu);

			unitHeader(rnglists);
			unitHeader(loclists);
			for(const DebugVar& g : di.globals)
				globalDie(g);
			for(const EncodedFunc& func : funcs)
				if(func.fn->getDebug())
					funcDie(func);
			for(U32 i = 0; i < pendingFuncs.size(); ++i)
				abstractDie(pendingFuncs[i]);
			// types last, emitting one may queue more
			for(U32 i = 0; i < pendingTypes.size(); ++i)
				typeDie(pendingTypes[i]);
			info.push_back(0);
			abbrev.push_back(0);

			for(const auto& [at, target] : refs)
				le::patch32(info, at, dieOff[target]);
			le::patch32(info, 0, (U32)info.size() - 4);
			le::patch32(rnglists, 0, (U32)rnglists.size() - 4);
			le::patch32(loclists, 0, (U32)loclists.size() - 4);
		}

		// unit_length, version 5, address_size, segment_selector_size, then the line table's
		// header_length or the lists' offset_entry_count
		void unitHeader(List<U8>& out) {
			le::put32(out, 0);
			out.insert(out.end(), {5, 0, kPtrBytes, 0, 0, 0, 0, 0});
		}

		void writeLineSequence(List<U8>& out, U32 at, const EncodedFunc& func, ObjectFile& obj) {
			out.insert(out.end(), {0, 1 + kPtrBytes, DW_LNE_set_address});
			String textSym = obj.sectionSymbol(ObjectFile::Text);
			U32 relocAt = at + (U32)out.size();
			obj.addReloc(ObjectFile::DebugLine, relocAt, textSym, RelocKind::Abs64, func.textOff);
			le::put64(out, 0);
			LineRow prev = {0, {1, 1, 0}}; // the state machine starts at file 1, line 1, column 0
			for(const LineRow& row : func.lines) {
				out.push_back(DW_LNS_advance_pc);
				le::uleb(out, row.at - prev.at);
				out.push_back(DW_LNS_set_file);
				le::uleb(out, row.loc.file);
				out.push_back(DW_LNS_set_column);
				le::uleb(out, row.loc.col);
				out.push_back(DW_LNS_advance_line);
				le::sleb(out, (I64)row.loc.line - (I64)prev.loc.line);
				out.push_back(DW_LNS_copy);
				prev = row;
			}
			out.push_back(DW_LNS_advance_pc);
			le::uleb(out, func.size - prev.at);
			out.insert(out.end(), {0, 1, DW_LNE_end_sequence});
		}

		void writeLineTable(const DebugInfo& di, const List<EncodedFunc>& funcs, ObjectFile& obj) {
			List<U8> out;
			unitHeader(out);
			U32 headerStart = (U32)out.size();
			// min_inst_length, max_ops, default_is_stmt, line_base -5, line_range, opcode_base, then the
			// standard opcode lengths
			out.insert(out.end(), {1, 1, 1, 0xfb, 14, 13, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1});
			out.insert(out.end(), {1, DW_LNCT_path, DW_FORM_string, 1});
			le::putString(out, di.compDir);
			out.insert(out.end(), {2, DW_LNCT_path, DW_FORM_string});
			out.insert(out.end(), {DW_LNCT_directory_index, DW_FORM_udata});
			le::uleb(out, di.files.size());
			for(const String& f : di.files) {
				le::putString(out, f);
				out.push_back(0);
			}
			le::patch32(out, headerStart - 4, (U32)out.size() - headerStart);
			U32 base = obj.sectionSize(ObjectFile::DebugLine);
			for(const EncodedFunc& func : funcs)
				if(func.fn->getDebug() && !func.lines.empty())
					writeLineSequence(out, base, func, obj);
			le::patch32(out, 0, (U32)out.size() - 4);
			obj.append(ObjectFile::DebugLine, out.data(), (U32)out.size());
		}

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
			le::padTo(out, start + alignUp(out.size() - start, kPtrBytes));
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
				U32 relocAt = base + (U32)out.size();
				obj.addReloc(ObjectFile::EhFrame, relocAt, textSym, RelocKind::Pc32, func.textOff);
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

	void writeDwarf(const Module& mod, const List<EncodedFunc>& funcs, ObjectFile& obj) {
		if(!funcs.empty())
			detail::writeEhFrame(funcs, obj);
		const DebugInfo* di = mod.getDebugInfo();
		if(!di)
			return;
		detail::DieWriter w(mod, obj);
		w.unit(funcs);
		obj.append(ObjectFile::DebugInfo, w.info.data(), (U32)w.info.size());
		obj.append(ObjectFile::DebugAbbrev, w.abbrev.data(), (U32)w.abbrev.size());
		obj.append(ObjectFile::DebugRnglists, w.rnglists.data(), (U32)w.rnglists.size());
		obj.append(ObjectFile::DebugLoclists, w.loclists.data(), (U32)w.loclists.size());
		detail::writeLineTable(*di, funcs, obj);
	}
} // namespace rat
