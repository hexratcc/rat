#include "linker_internal.h"

#include "elf_file.h"

namespace rat {
	using namespace elf;

	namespace detail {
		constexpr const C8* kInterp = "/lib64/ld-linux-x86-64.so.2";

		B32 isGotReloc(U32 t) {
			return t == R_X86_64_GOTPCREL || t == R_X86_64_GOTPCRELX || t == R_X86_64_REX_GOTPCRELX ||
						 t == R_X86_64_GOTTPOFF;
		}
	} // namespace detail

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
				 le::rd16(&img[16]) == ET_DYN) {
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
					if(obj.sections[r.secIdx].bucket == BDebug)
						continue; // debug refs never pull
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

	void detail::NeedTable::note(std::string_view name, B32 required) {
		auto it = index.find(name);
		if(it == index.end()) {
			std::string_view stable(names.internString(name.data(), name.size()), name.size());
			it = index.emplace(stable, (U32)needs.size()).first;
			needs.push_back({stable});
		}
		needs[it->second].required |= required;
	}

	// marks the needs lib defines, true when it defines any
	B32 detail::NeedTable::matchLibrary(const Lib& lib, U32& foundRequired) {
		B32 used = false;
		U32 n = (U32)(lib.dynsym.size() / 24);
		const C8* strs = (const C8*)lib.dynstr.data();
		for(U32 i = 0; i < n; ++i) {
			const U8* p = lib.dynsym.data() + (U64)i * 24;
			if(le::rd16(p + 6) == SHN_UNDEF)
				continue;
			U8 bind = (U8)(p[4] >> 4);
			if(bind != STB_GLOBAL && bind != STB_WEAK && bind != STB_GNU_UNIQUE)
				continue;
			U32 nameOff = le::rd32(p);
			if(nameOff >= lib.dynstr.size() || !strs[nameOff])
				continue;
			auto it = index.find(strs + nameOff);
			if(it == index.end() || needs[it->second].found)
				continue;
			Need& nd = needs[it->second];
			nd.found = true;
			nd.type = (U8)(p[4] & 0xf);
			nd.size = le::rd64(p + 16);
			used = true;
			if(nd.required)
				foundRequired++;
		}
		return used;
	}

	// loads libs until every required undefined name is found, records the used ones
	B32 Linker::resolveExternals() {
		detail::NeedTable table;
		table.note("__libc_start_main", true);
		for(const InObject& obj : objs) {
			for(const InRel& r : obj.rels) {
				const InSym& s = obj.syms[r.sym];
				if(obj.sections[r.secIdx].bucket == BDebug)
					continue; // unresolved debug refs read as 0
				if(s.undef && !globals.count(s.name) && !s.name.empty() && !detail::isLinkerSym(s.name))
					table.note(s.name, s.bind != STB_WEAK);
			}
		}
		U32 totalRequired = 0;
		for(const detail::Need& nd : table.needs)
			totalRequired += nd.required ? 1 : 0;
		U32 foundRequired = 0;
		for(const String& path : libFiles) {
			if(foundRequired == totalRequired)
				break;
			Lib lib;
			if(!loadLibrary(path, lib, err))
				return false;
			if(table.matchLibrary(lib, foundRequired))
				neededLibs.push_back(lib.soname);
		}
		return importNeeds(table);
	}

	B32 Linker::importNeeds(const detail::NeedTable& table) {
		for(const detail::Need& nd : table.needs) {
			String name(nd.name);
			if(!nd.found) {
				if(name == "__libc_start_main") {
					err = "libc is missing __libc_start_main";
					return false;
				}
				if(nd.required) {
					err = "undefined symbol '" + name + "'";
					return false;
				}
				continue;
			}
			B32 isData = nd.type == STT_OBJECT;
			importIndex.emplace(name, (U32)imports.size());
			imports.push_back({isData ? Import::Data : Import::Func, 0, 0, isData ? nd.size : 0});
			importNames.push_back(name);
		}
		return true;
	}

	// "L:obj:sym" for a local def, "G:name" otherwise
	String Linker::gotKey(U32 oi, const InRel& r) const {
		const InSym& s = objs[oi].syms[r.sym];
		if(!s.undef && s.bind == STB_LOCAL)
			return "L:" + std::to_string(oi) + ":" + std::to_string(r.sym);
		return "G:" + s.name;
	}

	// got slot per gotpcrel-family ref
	void Linker::assignGot() {
		for(U32 oi = 0; oi < objs.size(); ++oi) {
			const InObject& obj = objs[oi];
			for(const InRel& r : obj.rels) {
				if(!detail::isGotReloc(r.type))
					continue;
				String key = gotKey(oi, r);
				if(gotIndex.count(key))
					continue;
				const InSym& s = obj.syms[r.sym];
				GotSlot g;
				// a local or resolved global holds its own address; an import needs GLOB_DAT
				if(key[0] == 'G' && !globals.count(s.name)) {
					auto im = importIndex.find(s.name);
					if(im != importIndex.end()) {
						g.isImport = true;
						g.dynIndex = im->second; // patched in write()
					}
				}
				gotIndex.emplace(key, (U32)gotSlots.size());
				gotSlots.push_back(g);
			}
		}
	}

	B32 Linker::run() {
		interp = !opt.interp.empty() ? opt.interp : hostLoader();
		if(interp.empty())
			interp = detail::kInterp;
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
		layout();
		if(!applyRelocs())
			return false;
		if(size[OEhFrameHdr])
			buildEhFrameHdr();
		return write();
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
