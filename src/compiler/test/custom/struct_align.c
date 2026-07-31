// expect: 16
// expect-windows: 8
// Natural alignment: char, then long pads to the long's alignment.
struct Mixed { char c; long v; };

int main(void) {
	return sizeof(struct Mixed);
}
