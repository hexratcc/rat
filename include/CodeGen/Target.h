#ifndef RAT_CODEGEN_TARGET_H
#define RAT_CODEGEN_TARGET_H

#include "Core.h"

namespace rat {
	struct TargetInfo {
		enum class Endianness { Little, Big };

		virtual ~TargetInfo() = default;

		virtual const char* getName() const = 0;
		virtual U32 getPointerSizeInBits() const = 0;
		virtual Endianness getEndianness() const = 0;
		virtual U32 getNativeIntegerWidth() const = 0;

		U32 getPointerSizeInBytes() const { return getPointerSizeInBits() / 8; }
		B32 isLittleEndian() const { return getEndianness() == Endianness::Little; }
		B32 isBigEndian() const { return !isLittleEndian(); }
	};

	struct Generic64 final : TargetInfo {
		const char* getName() const override { return "generic64"; }
		U32 getPointerSizeInBits() const override { return 64; }
		Endianness getEndianness() const override { return Endianness::Little; }
		U32 getNativeIntegerWidth() const override { return 64; }
	};
} // namespace rat

#endif
