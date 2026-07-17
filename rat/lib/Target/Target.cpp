#include "Target/Target.h"

namespace rat {
	namespace {
		// linux:
		// - volatile:     rax rcx rdx rsi rdi r8-r11, xmm0-xmm15
		// - callee-saved: rbx r12-r15
		// windows:
		// - volatile:     rax rcx rdx r8-r11, xmm0-xmm5
		// - callee-saved: rbx rsi rdi r12-r15, xmm6-xmm15
		RegisterInfo buildX86Registers(OS os) {
			auto gp = [](U32 reg) -> PhysReg { return X86Target::kGpBase + reg; };
			auto xmm = [](U32 n) -> PhysReg { return X86Target::kXmmBase + n; };
			auto st = [](U32 n) -> PhysReg { return X86Target::kStBase + n; };

			RegClass gpc;
			gpc.id = X86Target::kGpClass;
			if(os == OS::Windows) {
				gpc.allocatable = {
						gp(0), gp(1), gp(2), gp(8), gp(9), gp(3), gp(6), gp(7), gp(12), gp(13), gp(14), gp(15)};
				gpc.calleeSaved = {gp(3), gp(6), gp(7), gp(12), gp(13), gp(14), gp(15)};
			} else {
				gpc.allocatable = {
						gp(0), gp(1), gp(2), gp(6), gp(7), gp(8), gp(9), gp(3), gp(12), gp(13), gp(14), gp(15)};
				gpc.calleeSaved = {gp(3), gp(12), gp(13), gp(14), gp(15)};
			}
			gpc.scratch = {gp(10), gp(11)};

			RegClass fpc;
			fpc.id = X86Target::kFpClass;
			if(os == OS::Windows) {
				for(U32 i = 0; i < 4; ++i)
					fpc.allocatable.push_back(xmm(i));
				fpc.scratch = {xmm(4), xmm(5)};
			} else {
				for(U32 i = 0; i < 14; ++i)
					fpc.allocatable.push_back(xmm(i));
				fpc.scratch = {xmm(14), xmm(15)};
			}

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

	RegisterInfo X86Target::build(OS os) { return buildX86Registers(os); }

	UniquePtr<TargetInfo> createTarget(const TargetTriple& triple) {
		return std::make_unique<X86Target>(triple);
	}
} // namespace rat
