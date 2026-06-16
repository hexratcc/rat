#ifndef RAT_CORE_H
#define RAT_CORE_H

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rat {
	using U8 = uint8_t;
	using U16 = uint16_t;
	using U32 = uint32_t;
	using U64 = uint64_t;

	using I8 = int8_t;
	using I16 = int16_t;
	using I32 = int32_t;
	using I64 = int64_t;

	using B32 = uint32_t;

	using String = std::string;
	template <typename Type> using List = std::vector<Type>;
	template <typename Type> using UniquePtr = std::unique_ptr<Type>;
	template <typename Key, typename Value>
	using Map = std::unordered_map<Key, Value>;
	template <typename Key> using Set = std::unordered_set<Key>;
} // namespace rat

#endif