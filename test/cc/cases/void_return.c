// expect: 5

// a void-returning function called purely for its side effect.
int g;
void setg(int v) { g = v; }

int main(void) {
	setg(5);
	return g;
}
