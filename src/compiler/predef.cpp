#include "predef.h"

namespace rat::cc {
	namespace {
		struct Def {
			const char* name;
			const char* value;
		};

		void append(String& out, const Def* defs, U32 n) {
			for(U32 i = 0; i < n; ++i) {
				out += "#define ";
				out += defs[i].name;
				if(defs[i].value && *defs[i].value) {
					out += ' ';
					out += defs[i].value;
				}
				out += '\n';
			}
		}

		template <U32 N> void append(String& out, const Def (&defs)[N]) { append(out, defs, N); }

		void appendCommon(String& out) {
			static const Def kDefs[] = {
					// language / conformance
					{"__STDC_VERSION__", "201112L"},
					{"__STDC_UTF_16__", "1"},
					{"__STDC_UTF_32__", "1"},
					{"__STDC_IEC_559__", "1"},
					{"__STDC_NO_ATOMICS__", "1"},
					{"__STDC_NO_THREADS__", "1"},
					{"__VERSION__", "\"ratcc 0.1\""},
					{"__NO_INLINE__", "1"},
					{"__GNUC__", "4"},
					{"__GNUC_MINOR__", "2"},
					{"__GNUC_PATCHLEVEL__", "1"},
					// architecture
					{"__x86_64", "1"},
					{"__x86_64__", "1"},
					{"__amd64", "1"},
					{"__amd64__", "1"},
					{"__MMX__", "1"},
					{"__FXSR__", "1"},
					{"__SSE__", "1"},
					{"__SSE2__", "1"},
					{"__SSE_MATH__", "1"},
					{"__SSE2_MATH__", "1"},
					{"__BIGGEST_ALIGNMENT__", "16"},
					// byte order
					{"__ORDER_LITTLE_ENDIAN__", "1234"},
					{"__ORDER_BIG_ENDIAN__", "4321"},
					{"__ORDER_PDP_ENDIAN__", "3412"},
					{"__BYTE_ORDER__", "__ORDER_LITTLE_ENDIAN__"},
					{"__FLOAT_WORD_ORDER__", "__ORDER_LITTLE_ENDIAN__"},
					// type sizes that do not vary between the supported targets
					{"__CHAR_BIT__", "8"},
					{"__SIZEOF_SHORT__", "2"},
					{"__SIZEOF_INT__", "4"},
					{"__SIZEOF_LONG_LONG__", "8"},
					{"__SIZEOF_POINTER__", "8"},
					{"__SIZEOF_SIZE_T__", "8"},
					{"__SIZEOF_PTRDIFF_T__", "8"},
					{"__SIZEOF_FLOAT__", "4"},
					{"__SIZEOF_DOUBLE__", "8"},
					{"__SIZEOF_LONG_DOUBLE__", "16"},
					// fixed-width limits
					{"__SCHAR_MAX__", "127"},
					{"__SHRT_MAX__", "32767"},
					{"__INT_MAX__", "2147483647"},
					{"__LONG_LONG_MAX__", "9223372036854775807LL"},
					{"__INTMAX_MAX__", "9223372036854775807LL"},
					{"__UINTMAX_MAX__", "18446744073709551615ULL"},
					{"__SIZE_MAX__", "18446744073709551615ULL"},
					{"__PTRDIFF_MAX__", "9223372036854775807LL"},
					{"__INTPTR_MAX__", "9223372036854775807LL"},
					{"__UINTPTR_MAX__", "18446744073709551615ULL"},
					{"__INT8_MAX__", "127"},
					{"__INT16_MAX__", "32767"},
					{"__INT32_MAX__", "2147483647"},
					{"__INT64_MAX__", "9223372036854775807LL"},
					{"__UINT8_MAX__", "255"},
					{"__UINT16_MAX__", "65535"},
					{"__UINT32_MAX__", "4294967295U"},
					{"__UINT64_MAX__", "18446744073709551615ULL"},
					{"__INT_LEAST8_MAX__", "127"},
					{"__INT_LEAST16_MAX__", "32767"},
					{"__INT_LEAST32_MAX__", "2147483647"},
					{"__INT_LEAST64_MAX__", "9223372036854775807LL"},
					{"__UINT_LEAST8_MAX__", "255"},
					{"__UINT_LEAST16_MAX__", "65535"},
					{"__UINT_LEAST32_MAX__", "4294967295U"},
					{"__UINT_LEAST64_MAX__", "18446744073709551615ULL"},
					{"__SIG_ATOMIC_MAX__", "2147483647"},
					{"__SIG_ATOMIC_MIN__", "(-2147483647 - 1)"},
					// widths
					{"__SCHAR_WIDTH__", "8"},
					{"__SHRT_WIDTH__", "16"},
					{"__INT_WIDTH__", "32"},
					{"__LONG_LONG_WIDTH__", "64"},
					{"__PTRDIFF_WIDTH__", "64"},
					{"__SIZE_WIDTH__", "64"},
					{"__INTMAX_WIDTH__", "64"},
					{"__UINTMAX_WIDTH__", "64"},
					{"__INTPTR_WIDTH__", "64"},
					{"__UINTPTR_WIDTH__", "64"},
					{"__SIG_ATOMIC_WIDTH__", "32"},
					// fixed-width types
					{"__INT8_TYPE__", "signed char"},
					{"__UINT8_TYPE__", "unsigned char"},
					{"__INT16_TYPE__", "short int"},
					{"__UINT16_TYPE__", "short unsigned int"},
					{"__INT32_TYPE__", "int"},
					{"__UINT32_TYPE__", "unsigned int"},
					{"__INT_LEAST8_TYPE__", "signed char"},
					{"__UINT_LEAST8_TYPE__", "unsigned char"},
					{"__INT_LEAST16_TYPE__", "short int"},
					{"__UINT_LEAST16_TYPE__", "short unsigned int"},
					{"__INT_LEAST32_TYPE__", "int"},
					{"__UINT_LEAST32_TYPE__", "unsigned int"},
					{"__INT_FAST8_TYPE__", "signed char"},
					{"__UINT_FAST8_TYPE__", "unsigned char"},
					{"__CHAR16_TYPE__", "short unsigned int"},
					{"__CHAR32_TYPE__", "unsigned int"},
					{"__SIG_ATOMIC_TYPE__", "int"},
					{"__INT8_C(c)", "c"},
					{"__UINT8_C(c)", "c"},
					{"__INT16_C(c)", "c"},
					{"__UINT16_C(c)", "c"},
					{"__INT32_C(c)", "c"},
					{"__UINT32_C(c)", "c ## U"},
					// float (IEEE binary32)
					{"__FLT_RADIX__", "2"},
					{"__FLT_EVAL_METHOD__", "0"},
					{"__FLT_MANT_DIG__", "24"},
					{"__FLT_DIG__", "6"},
					{"__FLT_MIN_EXP__", "(-125)"},
					{"__FLT_MAX_EXP__", "128"},
					{"__FLT_MIN_10_EXP__", "(-37)"},
					{"__FLT_MAX_10_EXP__", "38"},
					{"__FLT_DECIMAL_DIG__", "9"},
					{"__FLT_MAX__", "3.40282346638528859811704183484516925e+38F"},
					{"__FLT_MIN__", "1.17549435082228750796873653722224568e-38F"},
					{"__FLT_EPSILON__", "1.19209289550781250000000000000000000e-7F"},
					{"__FLT_DENORM_MIN__", "1.40129846432481707092992759355351247e-45F"},
					{"__FLT_HAS_DENORM__", "1"},
					{"__FLT_HAS_INFINITY__", "1"},
					{"__FLT_HAS_QUIET_NAN__", "1"},
					// double (IEEE binary64)
					{"__DBL_MANT_DIG__", "53"},
					{"__DBL_DIG__", "15"},
					{"__DBL_MIN_EXP__", "(-1021)"},
					{"__DBL_MAX_EXP__", "1024"},
					{"__DBL_MIN_10_EXP__", "(-307)"},
					{"__DBL_MAX_10_EXP__", "308"},
					{"__DBL_DECIMAL_DIG__", "17"},
					{"__DBL_MAX__", "1.79769313486231570814527423731704357e+308"},
					{"__DBL_MIN__", "2.22507385850720138309023271733240406e-308"},
					{"__DBL_EPSILON__", "2.22044604925031308084726333618164062e-16"},
					{"__DBL_DENORM_MIN__", "4.94065645841246544176568792868221372e-324"},
					{"__DBL_HAS_DENORM__", "1"},
					{"__DBL_HAS_INFINITY__", "1"},
					{"__DBL_HAS_QUIET_NAN__", "1"},
					// long double (x87 80-bit extended, stored in 16 bytes)
					{"__LDBL_MANT_DIG__", "64"},
					{"__LDBL_DIG__", "18"},
					{"__LDBL_MIN_EXP__", "(-16381)"},
					{"__LDBL_MAX_EXP__", "16384"},
					{"__LDBL_MIN_10_EXP__", "(-4931)"},
					{"__LDBL_MAX_10_EXP__", "4932"},
					{"__LDBL_DECIMAL_DIG__", "21"},
					{"__LDBL_MAX__", "1.18973149535723176502126385303097021e+4932L"},
					{"__LDBL_MIN__", "3.36210314311209350626267781732175260e-4932L"},
					{"__LDBL_EPSILON__", "1.08420217248550443400745280086994171e-19L"},
					{"__LDBL_DENORM_MIN__", "3.64519953188247460252840593361941982e-4951L"},
					{"__LDBL_HAS_DENORM__", "1"},
					{"__LDBL_HAS_INFINITY__", "1"},
					{"__LDBL_HAS_QUIET_NAN__", "1"},
					{"__DECIMAL_DIG__", "21"},
			};
			append(out, kDefs);
		}

		void appendLinux(String& out) {
			static const Def kDefs[] = {
					{"__linux", "1"},
					{"__linux__", "1"},
					{"linux", "1"},
					{"__gnu_linux__", "1"},
					{"__unix", "1"},
					{"__unix__", "1"},
					{"unix", "1"},
					{"__ELF__", "1"},
					{"__LP64__", "1"},
					{"_LP64", "1"},
					{"__STDC_ISO_10646__", "201706L"},
					{"__SIZEOF_LONG__", "8"},
					{"__SIZEOF_WCHAR_T__", "4"},
					{"__SIZEOF_WINT_T__", "4"},
					{"__LONG_MAX__", "9223372036854775807L"},
					{"__LONG_WIDTH__", "64"},
					{"__WCHAR_TYPE__", "int"},
					{"__WCHAR_MAX__", "2147483647"},
					{"__WCHAR_MIN__", "(-2147483647 - 1)"},
					{"__WCHAR_WIDTH__", "32"},
					{"__WINT_TYPE__", "unsigned int"},
					{"__WINT_MAX__", "4294967295U"},
					{"__WINT_MIN__", "0U"},
					{"__WINT_WIDTH__", "32"},
					{"__SIZE_TYPE__", "long unsigned int"},
					{"__PTRDIFF_TYPE__", "long int"},
					{"__INTPTR_TYPE__", "long int"},
					{"__UINTPTR_TYPE__", "long unsigned int"},
					{"__INTMAX_TYPE__", "long int"},
					{"__UINTMAX_TYPE__", "long unsigned int"},
					{"__INT64_TYPE__", "long int"},
					{"__UINT64_TYPE__", "long unsigned int"},
					{"__INT_LEAST64_TYPE__", "long int"},
					{"__UINT_LEAST64_TYPE__", "long unsigned int"},
					{"__INT_FAST16_TYPE__", "long int"},
					{"__UINT_FAST16_TYPE__", "long unsigned int"},
					{"__INT_FAST32_TYPE__", "long int"},
					{"__UINT_FAST32_TYPE__", "long unsigned int"},
					{"__INT_FAST64_TYPE__", "long int"},
					{"__UINT_FAST64_TYPE__", "long unsigned int"},
					{"__INT_FAST16_MAX__", "9223372036854775807L"},
					{"__INT_FAST32_MAX__", "9223372036854775807L"},
					{"__INT_FAST64_MAX__", "9223372036854775807L"},
					{"__UINT_FAST16_MAX__", "18446744073709551615UL"},
					{"__UINT_FAST32_MAX__", "18446744073709551615UL"},
					{"__UINT_FAST64_MAX__", "18446744073709551615UL"},
					{"__INT_FAST8_MAX__", "127"},
					{"__UINT_FAST8_MAX__", "255"},
					{"__INT64_C(c)", "c ## L"},
					{"__UINT64_C(c)", "c ## UL"},
					{"__INTMAX_C(c)", "c ## L"},
					{"__UINTMAX_C(c)", "c ## UL"},
			};
			append(out, kDefs);
		}

		void appendWindows(String& out) {
			static const Def kDefs[] = {
					{"_WIN32", "1"},
					{"_WIN64", "1"},
					{"WIN32", "1"},
					{"WIN64", "1"},
					{"__WIN32__", "1"},
					{"__WIN64__", "1"},
					{"__LLP64__", "1"},
					{"_INTEGRAL_MAX_BITS", "64"},
					{"__SIZEOF_LONG__", "4"},
					{"__SIZEOF_WCHAR_T__", "2"},
					{"__SIZEOF_WINT_T__", "2"},
					{"__LONG_MAX__", "2147483647L"},
					{"__LONG_WIDTH__", "32"},
					{"__WCHAR_UNSIGNED__", "1"},
					{"__WCHAR_TYPE__", "short unsigned int"},
					{"__WCHAR_MAX__", "65535"},
					{"__WCHAR_MIN__", "0"},
					{"__WCHAR_WIDTH__", "16"},
					{"__WINT_TYPE__", "short unsigned int"},
					{"__WINT_MAX__", "65535"},
					{"__WINT_MIN__", "0"},
					{"__WINT_WIDTH__", "16"},
					{"__SIZE_TYPE__", "long long unsigned int"},
					{"__PTRDIFF_TYPE__", "long long int"},
					{"__INTPTR_TYPE__", "long long int"},
					{"__UINTPTR_TYPE__", "long long unsigned int"},
					{"__INTMAX_TYPE__", "long long int"},
					{"__UINTMAX_TYPE__", "long long unsigned int"},
					{"__INT64_TYPE__", "long long int"},
					{"__UINT64_TYPE__", "long long unsigned int"},
					{"__INT_LEAST64_TYPE__", "long long int"},
					{"__UINT_LEAST64_TYPE__", "long long unsigned int"},
					{"__INT_FAST16_TYPE__", "short int"},
					{"__UINT_FAST16_TYPE__", "short unsigned int"},
					{"__INT_FAST32_TYPE__", "int"},
					{"__UINT_FAST32_TYPE__", "unsigned int"},
					{"__INT_FAST64_TYPE__", "long long int"},
					{"__UINT_FAST64_TYPE__", "long long unsigned int"},
					{"__INT_FAST16_MAX__", "32767"},
					{"__INT_FAST32_MAX__", "2147483647"},
					{"__INT_FAST64_MAX__", "9223372036854775807LL"},
					{"__UINT_FAST16_MAX__", "65535"},
					{"__UINT_FAST32_MAX__", "4294967295U"},
					{"__UINT_FAST64_MAX__", "18446744073709551615ULL"},
					{"__INT_FAST8_MAX__", "127"},
					{"__UINT_FAST8_MAX__", "255"},
					{"__INT64_C(c)", "c ## LL"},
					{"__UINT64_C(c)", "c ## ULL"},
					{"__INTMAX_C(c)", "c ## LL"},
					{"__UINTMAX_C(c)", "c ## ULL"},
			};
			append(out, kDefs);
		}

		String generate(const TargetTriple& t) {
			String out;
			out.reserve(8192);
			appendCommon(out);
			if(t.isWindows())
				appendWindows(out);
			else
				appendLinux(out);
			return out;
		}
	} // namespace

	const String& builtinPredefs(const TargetTriple& triple) {
		static const String linuxDefs = generate(TargetTriple(Arch::X86_64, OS::Linux));
		static const String windowsDefs = generate(TargetTriple(Arch::X86_64, OS::Windows));
		return triple.isWindows() ? windowsDefs : linuxDefs;
	}
} // namespace rat::cc
