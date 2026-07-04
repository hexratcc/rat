// expect: 0
// A global without an initializer is zero-initialized.
int g;

int main(void) {
	return g;
}
