#include "LinkerInternal.h"

#include "ElfFile.h"

#include <string_view>

namespace rat {
	using namespace elf;

	namespace {
		constexpr const char* kInterp = "/lib64/ld-linux-x86-64.so.2";
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
		if(!loadObject(std::move(img), p, obj, err))
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
					if(!loadObject(std::move(img), memPath, obj, err))
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
