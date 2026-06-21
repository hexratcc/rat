// expect: 42
// '&func' and explicit dereference '(*g)(...)' both yield the function.
int answer(int x) { return x; }
int main(void) {
	int (*g)(int) = &answer;
	return (*g)(42);
}
