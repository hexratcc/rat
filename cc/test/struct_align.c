// expect: 16
// skip-target: windows
// Natural alignment: char, then long forces 8-byte alignment + padding.
struct Mixed { char c; long v; };

int main(void) {
	return sizeof(struct Mixed);
}
