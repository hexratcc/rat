#ifndef RAT_TARGET_TARGET_H
#define RAT_TARGET_TARGET_H

#include "core.h"

namespace rat {
	struct MachineInstr;
	struct MachineFunc;
	struct X86CallConv;

	using PhysReg = U32;
	constexpr PhysReg kNoReg = 0;

	enum class Arch : U32 {
		X86_64
	};
	enum class OS : U32 {
		Linux,
		Windows
	};

	struct TargetTriple {
		Arch arch = Arch::X86_64;
		OS os = OS::Linux;

		TargetTriple() = default;
		TargetTriple(Arch a, OS o)
		: arch(a),
			os(o) {}

		B32 isWindows() const { return os == OS::Windows; }

		static B32 parse(const String& spec, TargetTriple& out, String& err);
	};

	struct RegClass {
		List<PhysReg> allocatable;
		List<PhysReg> calleeSaved;
		List<PhysReg> scratch;
		U32 spillBytes = 8;
	};

	constexpr U32 kMaxRegClasses = 4;

	struct RegisterInfo {
		List<RegClass> classes;
	};

	namespace detail {
		RegClass gpRegClass(const X86CallConv& conv);
		RegClass fpRegClass(const X86CallConv& conv);
	} // namespace detail

	struct RegAllocHooks {
		void (*makeReload)(MachineInstr& m, PhysReg dst, I32 slot, U32 cls, U32 width) = nullptr;
		void (*makeSpill)(MachineInstr& m, I32 slot, PhysReg src, U32 cls, U32 width) = nullptr;
		I32 (*allocSlot)(MachineFunc& fn, U32 cls, U32 width) = nullptr;
		B32 (*isCopy)(const MachineInstr& in) = nullptr;
	};

	struct TargetInfo {
		virtual ~TargetInfo() = default;

		virtual const TargetTriple& getTriple() const;
		virtual const RegisterInfo* registers() const { return nullptr; }
		virtual RegAllocHooks regAllocHooks() const;

		static constexpr U32 kPointerSizeInBits = 64;
		U32 getPointerSizeInBytes() const { return kPointerSizeInBits / 8; }
	};

	struct Generic64 final : TargetInfo {};

	struct X86Target final : TargetInfo {
		enum : PhysReg {
			kGpBase = 1,
			kXmmBase = 17,
			kRegEnd = 33, // past the last register
		};

		enum : U32 {
			kGpClass = 0,
			kFpClass = 1,
			kX87Class = 2, // its values live in frame slots, no register holds one
		};

		explicit X86Target(const TargetTriple& t);

		const TargetTriple& getTriple() const override { return triple; }

		const RegisterInfo* registers() const override { return &info; }

		RegAllocHooks regAllocHooks() const override;

		static B32 isXmm(PhysReg p) { return p >= kXmmBase && p < kRegEnd; }
	private:
		TargetTriple triple;
		RegisterInfo info;
	};
} // namespace rat

#endif
