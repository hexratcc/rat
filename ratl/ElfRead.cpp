#include "ElfRead.h"

#include <fstream>

namespace rat {
	B32 readWhole(const String& path, List<U8>& out) {
		std::ifstream f(path, std::ios::binary);
		if(!f)
			return false;
		f.seekg(0, std::ios::end);
		std::streamoff n = f.tellg();
		if(n < 0)
			return false;
		f.seekg(0, std::ios::beg);
		out.resize((U64)n);
		if(n > 0)
			f.read((char*)out.data(), n);
		return (B32)f.good() || f.eof();
	}
} // namespace rat
