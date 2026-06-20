// expect: 216
// File-scope arrays with inferred length, including a string-initialized char[].
int data[] = {1, 2, 3, 7};
char msg[] = "hi";
int main(void) {
	return data[3] + msg[0] + msg[1];
}
