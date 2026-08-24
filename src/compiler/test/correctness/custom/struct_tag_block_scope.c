// expect: 0

int main(void) {
	struct t {
		char a[2];
	};
	{
		struct t {
			char a[4];
		};
		if(sizeof(struct t) != 4)
			return 1;
	}
	struct t y;
	if(sizeof(y) != 2)
		return 2;
	// a body in the same block still completes a forward declaration
	struct u* p;
	struct u {
		int x;
	};
	(void)p;
	return sizeof(struct u) == 4 ? 0 : 3;
}
