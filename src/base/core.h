#ifndef RAT_CORE_H
#define RAT_CORE_H

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace rat {
	using U8 = uint8_t;
	using U16 = uint16_t;
	using U32 = uint32_t;
	using U64 = uint64_t;

	using I8 = int8_t;
	using I32 = int32_t;
	using I64 = int64_t;

	using F32 = float;
	using F64 = double;
	using F80 = long double;

	using C8 = char;
	using B32 = uint32_t;

	inline int32_t countTrailingZeros64(U64 v) {
#if defined(_MSC_VER)
		unsigned long index;
		_BitScanForward64(&index, v);
		return (int32_t)index;
#else
		return __builtin_ctzll(v);
#endif
	}

	inline int32_t countLeadingZeros64(U64 v) {
#if defined(_MSC_VER)
		unsigned long index;
		_BitScanReverse64(&index, v);
		return (int32_t)(63 - index);
#else
		return __builtin_clzll(v);
#endif
	}

	using String = std::string;
	template <typename Type> using List = std::vector<Type>;
	template <typename Key> using Set = std::unordered_set<Key>;
	template <typename Key, typename Value> using Map = std::unordered_map<Key, Value>;
	template <typename Type> using UniquePtr = std::unique_ptr<Type>;
	template <typename First, typename Second> using Pair = std::pair<First, Second>;
	template <typename Signature> using Delegate = std::function<Signature>;

	// list of trivially copyable values, inline up to N, then on the heap with the inline storage
	// holding the heap pointer
	template <typename T, U32 N> struct SmallList {
		static_assert(sizeof(T) * N >= sizeof(T*), "inline storage holds the heap pointer");

		SmallList() = default;
		SmallList(std::initializer_list<T> l) { assign(l.begin(), l.end()); }
		SmallList(const SmallList& o) { *this = o; }
		SmallList(SmallList&& o) noexcept { *this = std::move(o); }
		~SmallList() { release(); }

		SmallList& operator=(const SmallList& o) {
			if(o.cap != N)
				assign(o.begin(), o.end());
			else if(this != &o)
				take(o);
			return *this;
		}

		SmallList& operator=(SmallList&& o) noexcept {
			if(this != &o) {
				take(o);
				o.cap = N; // a heap block now belongs to this
				o.n = 0;
			}
			return *this;
		}

		T* begin() { return cap == N ? buf : heap(); }
		T* end() { return begin() + n; }
		const T* begin() const { return cap == N ? buf : heap(); }
		const T* end() const { return begin() + n; }
		U32 size() const { return n; }
		B32 empty() const { return n == 0; }
		T& operator[](U32 i) { return begin()[i]; }
		const T& operator[](U32 i) const { return begin()[i]; }
		T& back() { return begin()[n - 1]; }
		const T& back() const { return begin()[n - 1]; }
		void clear() { n = 0; }
		void pop_back() { --n; }

		void erase(T* first, T* last) {
			std::copy(last, end(), first);
			n -= (U32)(last - first);
		}

		void push_back(const T& x) {
			if(n == cap)
				reserve(2 * cap);
			begin()[n++] = x;
		}

		void assign(const T* b, const T* e) {
			n = 0;
			reserve((U32)(e - b));
			std::copy(b, e, begin());
			n = (U32)(e - b);
		}

		void reserve(U32 c) {
			if(c <= cap)
				return;
			T* p = new T[c];
			std::copy(begin(), end(), p);
			release();
			std::memcpy((void*)buf, (const void*)&p, sizeof(T*));
			cap = c;
		}
	private:
		T* heap() const {
			T* p;
			std::memcpy((void*)&p, (const void*)buf, sizeof(T*));
			return p;
		}

		void take(const SmallList& o) {
			release();
			std::memcpy((void*)buf, (const void*)o.buf, sizeof(buf));
			n = o.n;
			cap = o.cap;
		}

		void release() {
			if(cap != N)
				delete[] heap();
			cap = N;
		}

		T buf[N];
		U32 n = 0;
		U32 cap = N;
	};

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
			U64 v = reinterpret_cast<U64>(p);
			U64 a = align;
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

		template <typename T> T* makeArray(U64 count) {
			static_assert(std::is_trivially_destructible_v<T>, "arena arrays are never destroyed");
			if(count == 0)
				return nullptr;
			return static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
		}

		const C8* internString(const C8* s, U64 len) {
			C8* p = makeArray<C8>(len + 1);
			if(len)
				std::memcpy(p, s, len);
			p[len] = '\0';
			return p;
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