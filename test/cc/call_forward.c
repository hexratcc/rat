// expect: 15
// Call a function defined later in the translation unit (forward reference).
int main(void) {
	return triple(5);
}

int triple(int x) {
	return x * 3;
}
