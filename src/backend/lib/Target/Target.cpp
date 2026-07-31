#include "Target/Target.h"

#include "Target/X86Asm.h"

namespace rat {
	namespace {
		RegisterInfo buildX86Registers(const X86CallConv& conv) {
			auto gp = [](Reg reg) -> PhysReg { return X86Target::kGpBase + (PhysReg)reg; };
			auto xmm = [](U32 n) -> PhysReg { return X86Target::kXmmBase + n; };
			auto st = [](U32 n) -> PhysReg { return X86Target::kStBase + n; };
			auto calleeSaved = [&](Reg r) -> B32 {
				for(U32 i = 0; i < conv.gpCalleeSavedCount; ++i)
					if(conv.gpCalleeSaved[i] == r)
						return true;
				return false;
			};

			constexpr Reg kCandidates[] = {RAX, RCX, RDX, RBX, RSI, RDI, R8, R9, R12, R13, R14, R15};

			RegClass gpc;
			gpc.id = X86Target::kGpClass;
			for(U32 pass = 0; pass < 2; ++pass)
				for(Reg r : kCandidates)
					if(calleeSaved(r) == (pass == 1))
						gpc.allocatable.push_back(gp(r));
			for(U32 i = 0; i < conv.gpCalleeSavedCount; ++i)
				gpc.calleeSaved.push_back(gp(conv.gpCalleeSaved[i]));
			gpc.scratch = {gp(R10), gp(R11)};

			RegClass fpc;
			fpc.id = X86Target::kFpClass;
			for(U32 i = 0; i + 2 < conv.sseVolatileCount; ++i)
				fpc.allocatable.push_back(xmm(i));
			fpc.scratch = {xmm(conv.sseVolatileCount - 2), xmm(conv.sseVolatileCount - 1)};

			RegClass x87c;
			x87c.id = X86Target::kX87Class;
			for(U32 i = 0; i < 8; ++i) {
				x87c.allocatable.push_back(st(i));
				x87c.calleeSaved.push_back(st(i));
			}

			RegisterInfo info;
			info.classes = {gpc, fpc, x87c};
			info.spillSlotBytes = 8;
			return info;
		}
	} // namespace

	const C8* TargetTriple::archName() const {
		switch(arch) {
		case Arch::X86_64:
			return "x86_64";
		}
		return "unknown";
	}

	const C8* TargetTriple::osName() const {
		switch(os) {
		case OS::Linux:
			return "linux";
		case OS::Windows:
			return "windows";
		}
		return "unknown";
	}

	String TargetTriple::str() const { return String(archName()) + "-" + osName(); }

	B32 TargetTriple::parse(const String& spec, TargetTriple& out, String& err) {
		U32 dash = (U32)spec.find('-');
		// accept "x86-64"
		if(spec.rfind("x86-64-", 0) == 0)
			dash = 6;
		if(dash == (U32)String::npos || dash == 0 || dash + 1 >= spec.size()) {
			err = "malformed target triple '" + spec + "', expected <arch>-<os>";
			return false;
		}
		String archs = spec.substr(0, dash);
		String oss = spec.substr(dash + 1);

		if(archs == "x86_64" || archs == "x86-64" || archs == "amd64") {
			out.arch = Arch::X86_64;
		} else {
			err = "unknown architecture '" + archs + "'";
			return false;
		}
		if(oss == "linux" || oss == "gnu" || oss == "linux-gnu") {
			out.os = OS::Linux;
		} else if(oss == "windows" || oss == "win32") {
			out.os = OS::Windows;
		} else {
			err = "unknown operating system '" + oss + "'";
			return false;
		}
		return true;
	}

	const TargetTriple& TargetInfo::getTriple() const {
		static const TargetTriple kDefault(Arch::X86_64, OS::Linux);
		return kDefault;
	}

	RegAllocHooks TargetInfo::regAllocHooks() const { return {}; }

	RegisterInfo X86Target::build(OS os) { return buildX86Registers(x86CallConv(os)); }

	UniquePtr<TargetInfo> createTarget(const TargetTriple& triple) {
		return std::make_unique<X86Target>(triple);
	}
} // namespace rat
