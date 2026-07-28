#undef assert

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#if defined(_WIN32)
void _assert(const char* message, const char* file, unsigned line);
#define assert(expr) ((expr) ? (void)0 : _assert(#expr, __FILE__, __LINE__))
#else
void __assert_fail(const char* expr, const char* file, unsigned line, const char* func);
#define assert(expr) ((expr) ? (void)0 : __assert_fail(#expr, __FILE__, __LINE__, 0))
#endif
#endif

#define static_assert _Static_assert
