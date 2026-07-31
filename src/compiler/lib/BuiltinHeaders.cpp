#include "BuiltinHeaders.h"

#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace rat::cc {
	namespace {
		namespace fs = std::filesystem;

		// absolute path of the running executable, cross-platform
		fs::path selfExePath() {
#if defined(_WIN32)
			std::wstring buf(MAX_PATH, L'\0');
			for(;;) {
				DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
				if(n == 0)
					return {};
				if(n < buf.size()) {
					buf.resize(n);
					break;
				}
				buf.resize(buf.size() * 2);
			}
			return fs::path(buf);
#elif defined(__APPLE__)
			U32 size = 0;
			_NSGetExecutablePath(nullptr, &size);
			String buf(size, '\0');
			if(_NSGetExecutablePath(buf.data(), &size) != 0)
				return {};
			return fs::path(buf.c_str());
#else
			std::error_code ec;
			fs::path p = fs::read_symlink("/proc/self/exe", ec);
			return ec ? fs::path() : p;
#endif
		}
	} // namespace

	const String& builtinIncludeDir() {
		static const String dir = [] {
			fs::path exe = selfExePath();
			fs::path h = exe.parent_path() / ".." / "src" / "compiler" / "headers";
			return h.lexically_normal().generic_string();
		}();
		return dir;
	}
} // namespace rat::cc
