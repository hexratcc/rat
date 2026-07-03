#ifndef RAT_TARGET_TARGET_H
#define RAT_TARGET_TARGET_H

#include "Core.h"

namespace rat {
	struct MachineInstr;
	struct MachineFunc;

	using PhysReg = U32;
	constexpr PhysReg kNoReg = 0;

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
	};

	struct TargetInfo {
		virtual ~TargetInfo() = default;

		virtual const C8* getName() const = 0;
		virtual U32 getPointerSizeInBits() const = 0;
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

		const C8* getName() const override { return "x86-64"; }
		U32 getPointerSizeInBits() const override { return 64; }

		const RegisterInfo* registers() const override {
			static const RegisterInfo info = build();
			return &info;
		}

		RegAllocHooks regAllocHooks() const override;

		// classify a physical register by its encoding range.
		static B32 isXmm(PhysReg p) { return p >= kXmmBase && p < kStBase; }
	private:
		static RegisterInfo build() {
			auto gp = [](U32 reg) -> PhysReg { return kGpBase + reg; };
			auto xmm = [](U32 n) -> PhysReg { return kXmmBase + n; };
			auto st = [](U32 n) -> PhysReg { return kStBase + n; };

			RegClass gpc;
			gpc.id = kGpClass;
			gpc.allocatable = {
					gp(0), gp(1), gp(2), gp(6), gp(7), gp(8), gp(9), gp(3), gp(12), gp(13), gp(14), gp(15)};
			gpc.calleeSaved = {gp(3), gp(12), gp(13), gp(14), gp(15)};
			gpc.scratch = {gp(10), gp(11)};

			RegClass fpc;
			fpc.id = kFpClass;
			for(U32 i = 0; i < 14; ++i)
				fpc.allocatable.push_back(xmm(i));
			fpc.scratch = {xmm(14), xmm(15)};

			RegClass x87c;
			x87c.id = kX87Class;
			for(U32 i = 0; i < 8; ++i) {
				x87c.allocatable.push_back(st(i));
				x87c.calleeSaved.push_back(st(i));
			}

			RegisterInfo info;
			info.classes = {gpc, fpc, x87c};
			info.spillSlotBytes = 8;
			return info;
		}
	};
} // namespace rat

#endif
