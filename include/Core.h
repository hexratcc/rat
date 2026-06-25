#ifndef RAT_CORE_H
#define RAT_CORE_H

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <ostream>
#include <poll.h>
#include <queue>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

	using F32 = float;
	using F64 = double;

	using C8 = char;

	using B32 = uint32_t;

	using String = std::string;
	template <typename Type> using List = std::vector<Type>;
	template <typename Type> using UniquePtr = std::unique_ptr<Type>;
	template <typename Key, typename Value> using Map = std::unordered_map<Key, Value>;
	template <typename Key> using Set = std::unordered_set<Key>;

	inline I64 signExtend(I64 v, U32 w) {
		if(w == 0 || w >= 64)
			return v;
		U64 mask = ((U64)1 << w) - 1;
		U64 m = (U64)1 << (w - 1);
		U64 x = (U64)v & mask;
		return (I64)((x ^ m) - m);
	}

	namespace detail {
		constexpr U64 kDefaultChunk = 4096;

		inline C8* alignUp(C8* p, U64 align) {
			auto v = reinterpret_cast<std::uintptr_t>(p);
			std::uintptr_t a = align;
			return reinterpret_cast<C8*>((v + (a - 1)) & ~(a - 1));
		}
	} // namespace detail

	struct Arena {
		Arena() = default;
		~Arena() {
			for(auto it = dtors.rbegin(); it != dtors.rend(); ++it)
				it->run(it->obj);
		}

		Arena(const Arena&) = delete;
		Arena& operator=(const Arena&) = delete;

		template <typename T, typename... Args> T* make(Args&&... args) {
			void* mem = allocate(sizeof(T), alignof(T));
			T* obj = ::new (mem) T(std::forward<Args>(args)...);
			if constexpr(!std::is_trivially_destructible_v<T>)
				registerDtor(obj, [](void* p) { static_cast<T*>(p)->~T(); });
			return obj;
		}
	private:
		void* allocate(U64 size, U64 align) {
			C8* aligned = cur ? detail::alignUp(cur, align) : nullptr;
			if(!aligned || aligned + size > end) {
				U64 chunkSize = size + align > detail::kDefaultChunk ? size + align : detail::kDefaultChunk;
				chunks.push_back(UniquePtr<C8[]>(new C8[chunkSize]));
				cur = chunks.back().get();
				end = cur + chunkSize;
				aligned = detail::alignUp(cur, align);
			}
			cur = aligned + size;
			return aligned;
		}

		void registerDtor(void* obj, void (*dtor)(void*)) { dtors.push_back({obj, dtor}); }

		struct Dtor {
			void* obj;
			void (*run)(void*);
		};

		List<UniquePtr<C8[]>> chunks;
		C8* cur = nullptr;
		C8* end = nullptr;
		List<Dtor> dtors;
	};
} // namespace rat

#endif