#include "target/target.h"

#include "target/x86/x86_asm.h"

namespace rat {
	namespace detail {
		RegClass gpRegClass(const X86CallConv& conv) {
			constexpr Reg kCandidates[] = {RAX, RCX, RDX, RBX, RSI, RDI, R8, R9, R12, R13, R14, R15};
			PhysReg gp = X86Target::kGpBase;
			RegClass c;
			U32 calleeSavedBits = 0;
			for(U32 i = 0; i < conv.gpCalleeSavedCount; ++i) {
				c.calleeSaved.push_back(gp + conv.gpCalleeSaved[i]);
				calleeSavedBits |= 1u << conv.gpCalleeSaved[i];
			}
			// caller-saved first
			for(U32 wantCalleeSaved = 0; wantCalleeSaved < 2; ++wantCalleeSaved)
				for(Reg r : kCandidates)
					if(((calleeSavedBits >> r) & 1) == wantCalleeSaved)
						c.allocatable.push_back(gp + r);
			c.scratch = {gp + R10, gp + R11};
			return c;
		}

		RegClass fpRegClass(const X86CallConv& conv) {
			PhysReg xmm = X86Target::kXmmBase;
			RegClass c;
			for(U32 i = 0; i + 2 < conv.sseVolatileCount; ++i)
				c.allocatable.push_back(xmm + i);
			c.scratch = {xmm + conv.sseVolatileCount - 2, xmm + conv.sseVolatileCount - 1};
			c.spillBytes = 16; // an xmm vreg may hold a full 128-bit vector
			return c;
		}
	} // namespace detail

	X86Target::X86Target(const TargetTriple& t)
	: triple(t) {
		const X86CallConv& conv = x86CallConv(t.os);
		info.classes = {detail::gpRegClass(conv), detail::fpRegClass(conv)};
	}

	B32 TargetTriple::parse(const String& spec, TargetTriple& out, String& err) {
		// the arch may hold a dash, as in "x86-64"
		U32 dash = spec.rfind("x86-64-", 0) == 0 ? 6 : (U32)spec.find('-');
		if(dash == (U32)String::npos || dash == 0 || dash + 1 >= spec.size()) {
			err = "malformed target triple '" + spec + "', expected <arch>-<os>";
			return false;
		}
		String archs = spec.substr(0, dash);
		String oss = spec.substr(dash + 1);
		if(archs != "x86_64" && archs != "x86-64" && archs != "amd64") {
			err = "unknown architecture '" + archs + "'";
			return false;
		}
		out.arch = Arch::X86_64;
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
} // namespace rat
