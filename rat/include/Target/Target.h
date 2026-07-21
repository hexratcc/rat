#ifndef RAT_TARGET_TARGET_H
#define RAT_TARGET_TARGET_H

#include "Core.h"

namespace rat {
	struct MachineInstr;
	struct MachineFunc;

	using PhysReg = U32;
	constexpr PhysReg kNoReg = 0;

	enum class Arch : U32 { X86_64 };
	enum class OS : U32 { Linux, Windows };

	struct TargetTriple {
		Arch arch = Arch::X86_64;
		OS os = OS::Linux;

		TargetTriple() = default;
		TargetTriple(Arch a, OS o)
		: arch(a),
			os(o) {}

		B32 isWindows() const { return os == OS::Windows; }
		B32 isLinux() const { return os == OS::Linux; }

		const C8* archName() const;
		const C8* osName() const;
		String str() const;

		static B32 parse(const String& spec, TargetTriple& out, String& err);
	};

	struct RegClass {
		U32 id = 0;
		List<PhysReg> allocatable;
		List<PhysReg> calleeSaved;
		List<PhysReg> scratch;
	};

	struct RegisterInfo {
		List<RegClass> classes;
		U32 spillSlotBytes = 8;
	};

	struct RegAllocHooks {
		Delegate<MachineInstr(PhysReg dst, I32 slot, U32 cls, U32 width)> makeReload;
		Delegate<MachineInstr(I32 slot, PhysReg src, U32 cls, U32 width)> makeSpill;
		Delegate<I32(MachineFunc& fn, U32 cls, U32 width)> allocSlot;
		Delegate<B32(const MachineInstr&)> isCopy;
		Delegate<B32(const MachineInstr&)> isRemat;
	};

	struct TargetInfo {
		virtual ~TargetInfo() = default;

		virtual const C8* getName() const = 0;
		virtual U32 getPointerSizeInBits() const = 0;
		virtual const TargetTriple& getTriple() const;
		virtual const RegisterInfo* registers() const { return nullptr; }
		virtual RegAllocHooks regAllocHooks() const;

		U32 getPointerSizeInBytes() const { return getPointerSizeInBits() / 8; }
	};

	struct Generic64 final : TargetInfo {
		const C8* getName() const override { return "generic64"; }
		U32 getPointerSizeInBits() const override { return 64; }
	};

	struct X86Target final : TargetInfo {
		enum : PhysReg {
			kGpBase = 1,
			kXmmBase = 17,
			kStBase = 33,
		};

		enum : U32 {
			kGpClass = 0,
			kFpClass = 1,
			kX87Class = 2,
		};

		explicit X86Target(OS os = OS::Linux)
		: triple(Arch::X86_64, os),
			info(build(os)) {}

		explicit X86Target(const TargetTriple& t)
		: triple(t),
			info(build(t.os)) {}

		const C8* getName() const override { return "x86-64"; }
		U32 getPointerSizeInBits() const override { return 64; }
		const TargetTriple& getTriple() const override { return triple; }

		const RegisterInfo* registers() const override { return &info; }

		RegAllocHooks regAllocHooks() const override;

		// classify a physical register by its encoding range.
		static B32 isXmm(PhysReg p) { return p >= kXmmBase && p < kStBase; }
	private:
		static RegisterInfo build(OS os);

		TargetTriple triple;
		RegisterInfo info;
	};

	UniquePtr<TargetInfo> createTarget(const TargetTriple& triple);
} // namespace rat

#endif
