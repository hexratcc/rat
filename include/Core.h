#ifndef RAT_CORE_H
#define RAT_CORE_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <ostream>

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

	namespace detail {
		constexpr std::size_t kDefaultChunk = 4096;

		inline char* alignUp(char* p, std::size_t align) {
			auto v = reinterpret_cast<std::uintptr_t>(p);
			std::uintptr_t a = align;
			return reinterpret_cast<char*>((v + (a - 1)) & ~(a - 1));
		}
	} // namespace detail

	struct Arena {
		Arena() = default;
		~Arena() {
			for (auto it = dtors.rbegin(); it != dtors.rend(); ++it)
				it->run(it->obj);
		}

		Arena(const Arena&) = delete;
		Arena& operator=(const Arena&) = delete;

		template <typename T, typename... Args> T* make(Args&&... args) {
			void* mem = allocate(sizeof(T), alignof(T));
			T* obj = ::new (mem) T(std::forward<Args>(args)...);
			if constexpr (!std::is_trivially_destructible_v<T>)
				registerDtor(obj, [](void* p) { static_cast<T*>(p)->~T(); });
			return obj;
		}

	private:
		void* allocate(std::size_t size, std::size_t align) {
			char* aligned = cur ? detail::alignUp(cur, align) : nullptr;
			if (!aligned || aligned + size > end) {
				std::size_t chunkSize = size + align > detail::kDefaultChunk
																		? size + align
																		: detail::kDefaultChunk;
				chunks.push_back(UniquePtr<char[]>(new char[chunkSize]));
				cur = chunks.back().get();
				end = cur + chunkSize;
				aligned = detail::alignUp(cur, align);
			}
			cur = aligned + size;
			return aligned;
		}

		void registerDtor(void* obj, void (*dtor)(void*)) {
			dtors.push_back({obj, dtor});
		}

		struct Dtor {
			void* obj;
			void (*run)(void*);
		};

		List<UniquePtr<char[]>> chunks;
		char* cur = nullptr;
		char* end = nullptr;
		List<Dtor> dtors;
	};
} // namespace rat

#endif