#include "LinkerInternal.h"

#include "ElfFile.h"

#include <string_view>

namespace rat {
	using namespace elf;

	namespace {
		constexpr const char* kInterp = "/lib64/ld-linux-x86-64.so.2";

		B32 isLinkerSym(const String& n) {
			static const char* names[] = {"__dso_handle",
																		"_GLOBAL_OFFSET_TABLE_",
																		"__init_array_start",
																		"__init_array_end",
																		"__preinit_array_start",
																		"__preinit_array_end",
																		"__fini_array_start",
																		"__fini_array_end",
																		"__bss_start",
																		"_edata",
																		"edata",
																		"_end",
																		"end",
																		"__end__",
																		"__ehdr_start",
																		"_DYNAMIC",
																		"__data_start",
																		"data_start",
																		"__TMC_END__"};
			for(const char* s : names)
				if(n == s)
					return true;
			return false;
		}
	} // namespace

	Import& Linker::intern(const String& name, Import::Kind kind) {
		auto it = importIndex.find(name);
		if(it != importIndex.end())
			return imports[it->second];
		U32 idx = (U32)imports.size();
		importIndex.emplace(name, idx);
		Import im;
		im.kind = kind;
		imports.push_back(im);
		importNames.push_back(name);
		return imports.back();
	}

	B32 Linker::loadInputs() {
		if(opt.inputs.empty()) {
			err = "no input objects";
			return false;
		}
		Set<String> seenInputs;
		for(const String& p : opt.inputs) {
			// collapse repeated paths
			if(!seenInputs.insert(p).second)
				continue;
			List<U8> img;
			if(!readWhole(p, img)) {
				err = "cannot read '" + p + "'";
				return false;
			}
			if(img.size() >= 8 && memcmp(img.data(), "!<arch>\n", 8) == 0) {
				ArchiveFile ar;
				if(!parseArchive(p, std::move(img), ar, err))
					return false;
				archives.push_back(std::move(ar));
				continue;
			}
			// positional shared object is a lib
			if(img.size() >= 18 && img[0] == 0x7f && img[1] == 'E' && img[2] == 'L' && img[3] == 'F' &&
				 rd16(&img[16]) == ET_DYN) {
				libFiles.push_back(p);
				continue;
			}
			InObject obj;
		if(!loadObject(std::move(img), p, obj, seenGroups, err))
				return false;
			objs.push_back(std::move(obj));
		}
		return true;
	}

	B32 Linker::pullArchives() {
		// lazily pull members satisfying undefined syms
		if(archives.empty())
			return true;
		B32 changed = true;
		while(changed) {
			changed = false;
			// names defined so far
			Set<std::string_view> def;
			for(const InObject& obj : objs)
				for(const InSym& s : obj.syms)
					if(!s.undef && s.bind != STB_LOCAL && !s.abs)
						def.insert(s.name);
			// referenced but not yet defined
			Set<std::string_view> undef;
			for(const InObject& obj : objs)
				for(const InRel& r : obj.rels) {
					const InSym& s = obj.syms[r.sym];
					if(s.undef && !s.name.empty() && !def.count(s.name))
						undef.insert(s.name);
				}
			for(ArchiveFile& ar : archives) {
				for(std::string_view nm : undef) {
					auto it = ar.index.find(String(nm));
					if(it == ar.index.end() || ar.pulled.count(it->second))
						continue;
					U64 memOff = it->second;
					U64 dataOff = memOff + 60;
					U64 sz = arMemberSize(ar.data, memOff);
					if(dataOff + sz > ar.data.size())
						continue;
					List<U8> img(ar.data.begin() + dataOff, ar.data.begin() + dataOff + sz);
					InObject obj;
					String memPath = ar.path + "(" + std::to_string(memOff) + ")";
					if(!loadObject(std::move(img), memPath, obj, seenGroups, err))
						return false;
					objs.push_back(std::move(obj));
					ar.pulled.insert(memOff);
					changed = true;
				}
			}
		}
		return true;
	}

	B32 Linker::loadLibraries() {
		List<String> want = opt.libs.empty() ? List<String>{"c", "m"} : opt.libs;
		// _start calls __libc_start_main, so libc is always needed
		B32 haveC = false;
		for(const String& w : want)
			if(w == "c")
				haveC = true;
		if(!haveC)
			want.push_back("c");
		List<String> paths = opt.libPaths;
		// host lib dirs (nix/non-fhs) then fhs fallbacks
		for(const String& d : hostLibDirs())
			paths.push_back(d);
		paths.push_back("/lib/x86_64-linux-gnu");
		paths.push_back("/usr/lib/x86_64-linux-gnu");
		paths.push_back("/lib64");
		paths.push_back("/usr/lib64");
		paths.push_back("/usr/lib");
		paths.push_back("/lib");
		for(const String& l : want) {
			String path;
			if(!findLibrary(l, paths, path)) {
				err = "cannot find library -l" + l;
				return false;
			}
			libFiles.push_back(std::move(path));
		}
		return true;
	}

	void Linker::collectGlobals() {
		// pick defining object per global
		for(U32 oi = 0; oi < objs.size(); ++oi) {
			const InObject& obj = objs[oi];
			for(U32 si = 0; si < obj.syms.size(); ++si) {
				const InSym& s = obj.syms[si];
				if(s.undef || s.bind == STB_LOCAL || s.abs)
					continue;
				Def& d = globals[s.name];
				if(s.common) {
					if(d.defined && !d.common)
						continue; // real def beats tentative
					d.common = true;
					d.defined = true;
					if(s.size > d.comSize)
						d.comSize = s.size;
					if(s.value > d.comAlign)
						d.comAlign = s.value ? s.value : 1;
					d.isFunc = false;
					continue;
				}
				B32 weak = s.bind == STB_WEAK;
				if(d.defined && !d.common) {
					// only a strong replaces an existing weak
					B32 existingWeak = d.obj != 0xffffffffu && objs[d.obj].syms[d.sym].bind == STB_WEAK;
					if(!(existingWeak && !weak))
						continue;
				}
				d.common = false;
				d.defined = true;
				d.obj = oi;
				d.sym = si;
				d.isFunc = s.type == STT_FUNC || s.type == STT_GNU_IFUNC;
				d.isTls = s.type == STT_TLS;
			}
		}
	}

	B32 Linker::resolveExternals() {
		struct Need {
			B32 required = false;
			B32 found = false;
			U8 type = STT_NOTYPE;
			U64 size = 0;
		};
		Arena names;
		Map<std::string_view, U32> needIndex;
		List<String> needNames;
		List<Need> needs;
		auto note = [&](std::string_view name) -> Need& {
			auto it = needIndex.find(name);
			if(it != needIndex.end())
				return needs[it->second];
			const C8* stable = names.internString(name.data(), name.size());
			U32 idx = (U32)needNames.size();
			needIndex.emplace(std::string_view(stable, name.size()), idx);
			needNames.emplace_back(name);
			needs.push_back({});
			return needs[idx];
		};
		note("__libc_start_main").required = true;

		for(const InObject& obj : objs) {
			for(const InRel& r : obj.rels) {
				const InSym& s = obj.syms[r.sym];
				if(!s.undef || globals.count(s.name))
					continue;
				if(s.name.empty() || isLinkerSym(s.name))
					continue;
				if(s.bind != STB_WEAK)
					note(s.name).required = true;
				else
					note(s.name);
			}
		}
		U32 totalRequired = 0;
		for(const Need& nd : needs)
			totalRequired += nd.required ? 1 : 0;

		U32 foundRequired = 0;
		for(const String& path : libFiles) {
			if(foundRequired == totalRequired)
				break;
			Lib lib;
			if(!loadLibrary(path, lib, err))
				return false;
			B32 used = false;
			U32 n = (U32)(lib.dynsym.size() / 24);
			const U8* syms = lib.dynsym.data();
			const char* strs = (const char*)lib.dynstr.data();
			U64 strMax = lib.dynstr.size();
			for(U32 i = 0; i < n; ++i) {
				const U8* p = syms + (U64)i * 24;
				if(rd16(p + 6) == SHN_UNDEF)
					continue;
				U8 bind = (U8)(p[4] >> 4);
				if(bind != STB_GLOBAL && bind != STB_WEAK && bind != STB_GNU_UNIQUE)
					continue;
				U32 nameOff = rd32(p);
				if(nameOff >= strMax || !strs[nameOff])
					continue;
				auto it = needIndex.find(strs + nameOff);
				if(it == needIndex.end() || needs[it->second].found)
					continue;
				Need& nd = needs[it->second];
				nd.found = true;
				nd.type = (U8)(p[4] & 0xf);
				nd.size = rd64(p + 16);
				used = true;
				if(nd.required)
					foundRequired++;
			}
			if(used)
				neededLibs.push_back(lib.soname);
		}

		for(U32 i = 0; i < needNames.size(); ++i) {
			const Need& nd = needs[i];
			if(!nd.found) {
				if(needNames[i] == "__libc_start_main") {
					err = "libc is missing __libc_start_main";
					return false;
				}
				if(nd.required) {
					err = "undefined symbol '" + needNames[i] + "'";
					return false;
				}
				continue;
			}
			B32 isData = nd.type == STT_OBJECT;
			Import& im = intern(needNames[i], isData ? Import::Data : Import::Func);
			if(isData)
				im.size = nd.size;
		}
		return true;
	}

	void Linker::assignGot() {
		// got slot per gotpcrel-family ref
		auto want = [](U32 t) {
			return t == R_X86_64_GOTPCREL || t == R_X86_64_GOTPCRELX || t == R_X86_64_REX_GOTPCRELX ||
						 t == R_X86_64_GOTTPOFF;
		};
		for(U32 oi = 0; oi < objs.size(); ++oi) {
			const InObject& obj = objs[oi];
			for(const InRel& r : obj.rels) {
				if(!want(r.type))
					continue;
				const InSym& s = obj.syms[r.sym];
				String key;
				B32 isImport = false;
				U32 dynIdx = 0;
				if(!s.undef && s.bind == STB_LOCAL) {
					key = "L:" + std::to_string(oi) + ":" + std::to_string(r.sym);
				} else if(globals.count(s.name)) {
					key = "G:" + s.name;
				} else {
					auto im = importIndex.find(s.name);
					if(im != importIndex.end()) {
						key = "G:" + s.name;
						isImport = true;
						dynIdx = im->second; // dynindex resolved after layout
					} else {
						key = "G:" + s.name; // unresolved weak, defined 0
					}
				}
				if(gotIndex.count(key))
					continue;
				U32 idx = (U32)gotSlots.size();
				gotIndex.emplace(key, idx);
				GotSlot g;
				g.isImport = isImport;
				g.defined = !isImport;
				if(isImport)
					g.dynIndex = dynIdx; // patched in write()
				gotSlots.push_back(g);
			}
		}
	}

	void Linker::buildStart() {
		auto emit = [&](std::initializer_list<U8> b) {
			for(U8 x : b)
				startCode.push_back(x);
		};
		emit({0x31, 0xed});							// xor  ebp, ebp
		emit({0x49, 0x89, 0xd1});				// mov  r9, rdx
		emit({0x5e});										// pop  rsi
		emit({0x48, 0x89, 0xe2});				// mov  rdx, rsp
		emit({0x48, 0x83, 0xe4, 0xf0}); // and  rsp, -16
		emit({0x50});										// push rax
		emit({0x54});										// push rsp
		emit({0x45, 0x31, 0xc0});				// xor  r8d, r8d
		emit({0x31, 0xc9});							// xor  ecx, ecx
		emit({0x48, 0x8d, 0x3d});				// lea  rdi, [rip+disp32]
		startLeaDisp = startCode.size();
		emit({0, 0, 0, 0});
		emit({0xe8}); // call rel32
		startCallDisp = startCode.size();
		emit({0, 0, 0, 0});
		emit({0xf4}); // hlt
	}

	B32 Linker::run() {
		interp = !opt.interp.empty() ? opt.interp : hostLoader();
		if(interp.empty())
			interp = kInterp;
		rpaths = !opt.rpaths.empty() ? opt.rpaths : hostLibDirs();
		if(!loadInputs())
			return false;
		if(!pullArchives())
			return false;
		if(!loadLibraries())
			return false;
		collectGlobals();
		if(!resolveExternals())
			return false;
		assignGot();
		buildStart();
		layout();
		if(!applyRelocs())
			return false;
		if(size[OEhFrameHdr])
			buildEhFrameHdr();
		return true;
	}

	B32 link(const LinkOptions& opt, String& err) {
		switch(opt.target) {
		case LinkTarget::LinuxX64: {
			Linker linker(opt);
			if(linker.run())
				return true;
			err = linker.err;
			return false;
		}
		case LinkTarget::WindowsX64:
			err = "Windows/x86-64 linking is not implemented yet";
			return false;
		}
		err = "unknown link target";
		return false;
	}
} // namespace rat
