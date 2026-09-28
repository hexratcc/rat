#include "target/target.h"

#include "target/x86/x86_asm.h"

namespace rat {
	namespace detail {
		B32 isCalleeSaved(const X86CallConv& conv, Reg r) {
			const Reg* end = conv.gpCalleeSaved + conv.gpCalleeSavedCount;
			return std::find(conv.gpCalleeSaved, end, r) != end;
		}

		RegClass gpRegClass(const X86CallConv& conv) {
			constexpr Reg kCandidates[] = {RAX, RCX, RDX, RBX, RSI, RDI, R8, R9, R12, R13, R14, R15};
			PhysReg gp = X86Target::kGpBase;
			RegClass c;
			c.id = X86Target::kGpClass;
			for(U32 pass = 0; pass < 2; ++pass)
				for(Reg r : kCandidates)
					if(isCalleeSaved(conv, r) == (pass == 1))
						c.allocatable.push_back(gp + r);
			for(U32 i = 0; i < conv.gpCalleeSavedCount; ++i)
				c.calleeSaved.push_back(gp + conv.gpCalleeSaved[i]);
			c.scratch = {gp + R10, gp + R11};
			return c;
		}

		RegClass fpRegClass(const X86CallConv& conv) {
			PhysReg xmm = X86Target::kXmmBase;
			RegClass c;
			c.id = X86Target::kFpClass;
			for(U32 i = 0; i + 2 < conv.sseVolatileCount; ++i)
				c.allocatable.push_back(xmm + i);
			c.scratch = {xmm + conv.sseVolatileCount - 2, xmm + conv.sseVolatileCount - 1};
			c.spillBytes = 16; // an xmm vreg may hold a full 128-bit vector
			return c;
		}

		RegClass x87RegClass() {
			RegClass c;
			c.id = X86Target::kX87Class;
			for(U32 i = 0; i < 8; ++i) {
				c.allocatable.push_back(X86Target::kStBase + i);
				c.calleeSaved.push_back(X86Target::kStBase + i);
			}
			return c;
		}

		RegisterInfo buildX86Registers(const X86CallConv& conv) {
			RegisterInfo info;
			info.classes = {gpRegClass(conv), fpRegClass(conv), x87RegClass()};
			info.spillSlotBytes = 8;
			return info;
		}
	} // namespace detail

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

	RegisterInfo X86Target::build(OS os) { return detail::buildX86Registers(x86CallConv(os)); }

	UniquePtr<TargetInfo> createTarget(const TargetTriple& triple) {
		return std::make_unique<X86Target>(triple);
	}
} // namespace rat
