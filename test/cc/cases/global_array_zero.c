// expect: 14
// An uninitialized file-scope array is zero-filled and writable.
int buf[4];
int main(void) {
	buf[0] = 5;
	buf[2] = 9;
	return buf[0] + buf[2] + buf[1];
}
