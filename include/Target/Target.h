#ifndef RAT_TARGET_TARGET_H
#define RAT_TARGET_TARGET_H

#include "Core.h"

namespace rat {
	using PhysReg = U32;
	constexpr PhysReg kNoReg = 0;

	struct RegClass {
		U32 id = 0;
		const C8* name = "";
		List<PhysReg> allocatable;
		List<PhysReg> callerSaved;
		List<PhysReg> calleeSaved;
		List<PhysReg> scratch;
	};

	struct RegisterInfo {
		List<RegClass> classes;
		U32 spillSlotBytes = 8;
		const C8* (*regName)(PhysReg) = nullptr;
	};

	struct TargetInfo {
		enum class Endianness { Little, Big };

		virtual ~TargetInfo() = default;

		virtual const C8* getName() const = 0;
		virtual U32 getPointerSizeInBits() const = 0;
		virtual Endianness getEndianness() const = 0;
		virtual U32 getNativeIntegerWidth() const = 0;
		virtual const RegisterInfo* registers() const { return nullptr; }

		U32 getPointerSizeInBytes() const { return getPointerSizeInBits() / 8; }
		B32 isLittleEndian() const { return getEndianness() == Endianness::Little; }
		B32 isBigEndian() const { return !isLittleEndian(); }
	};

	struct Generic64 final : TargetInfo {
		const C8* getName() const override { return "generic64"; }
		U32 getPointerSizeInBits() const override { return 64; }
		Endianness getEndianness() const override { return Endianness::Little; }
		U32 getNativeIntegerWidth() const override { return 64; }
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
		Endianness getEndianness() const override { return Endianness::Little; }
		U32 getNativeIntegerWidth() const override { return 64; }

		const RegisterInfo* registers() const override {
			static const RegisterInfo info = build();
			return &info;
		}

		// classify a physical register by its encoding range.
		static B32 isGp(PhysReg p) { return p >= kGpBase && p < kXmmBase; }
		static B32 isXmm(PhysReg p) { return p >= kXmmBase && p < kStBase; }

		static const C8* nameOf(PhysReg r) {
			static const C8* gp[16] = {"rax",
																 "rcx",
																 "rdx",
																 "rbx",
																 "rsp",
																 "rbp",
																 "rsi",
																 "rdi",
																 "r8",
																 "r9",
																 "r10",
																 "r11",
																 "r12",
																 "r13",
																 "r14",
																 "r15"};
			if(r >= kGpBase && r < kGpBase + 16)
				return gp[r - kGpBase];
			if(r >= kXmmBase && r < kXmmBase + 16) {
				static C8 buf[8];
				std::snprintf(buf, sizeof(buf), "xmm%u", r - kXmmBase);
				return buf;
			}
			if(r >= kStBase && r < kStBase + 8) {
				static C8 buf[8];
				std::snprintf(buf, sizeof(buf), "st%u", r - kStBase);
				return buf;
			}
			return "?";
		}
	private:
		static RegisterInfo build() {
			auto gp = [](U32 reg) -> PhysReg { return kGpBase + reg; };
			auto xmm = [](U32 n) -> PhysReg { return kXmmBase + n; };
			auto st = [](U32 n) -> PhysReg { return kStBase + n; };

			RegClass gpc;
			gpc.id = kGpClass;
			gpc.name = "gp";
			gpc.allocatable = {
					gp(0), gp(1), gp(2), gp(6), gp(7), gp(8), gp(9), gp(3), gp(12), gp(13), gp(14), gp(15)};
			gpc.callerSaved = {gp(0), gp(1), gp(2), gp(6), gp(7), gp(8), gp(9)};
			gpc.calleeSaved = {gp(3), gp(12), gp(13), gp(14), gp(15)};
			gpc.scratch = {gp(10), gp(11)};

			RegClass fpc;
			fpc.id = kFpClass;
			fpc.name = "fp";
			for(U32 i = 0; i < 14; ++i) {
				fpc.allocatable.push_back(xmm(i));
				fpc.callerSaved.push_back(xmm(i));
			}
			fpc.scratch = {xmm(14), xmm(15)};

			RegClass x87c;
			x87c.id = kX87Class;
			x87c.name = "x87";
			for(U32 i = 0; i < 8; ++i) {
				x87c.allocatable.push_back(st(i));
				x87c.calleeSaved.push_back(st(i));
			}

			RegisterInfo info;
			info.classes = {gpc, fpc, x87c};
			info.spillSlotBytes = 8;
			info.regName = &X86Target::nameOf;
			return info;
		}
	};
} // namespace rat

#endif
