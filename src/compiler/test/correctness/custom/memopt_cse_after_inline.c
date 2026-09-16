// expect: 0
// passes: gvn memoryopt inline

// inlining clones the body of get() into main with fresh, higher node ids, so
// the clone's load of G sits earlier in the memory chain than main's own load
// of G but carries the larger id
struct R {
	long a, b;
};

static struct R G = {1, 2};

static struct R get(void) { return G; }

int main(void) {
	struct R r = get();
	if(r.a != G.a || r.b != G.b)
		return 1;
	return 0;
}
