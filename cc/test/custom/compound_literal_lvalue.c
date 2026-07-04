// expect: 22
// A compound literal at block scope designates an unnamed object with automatic
// storage duration and is a modifiable lvalue (C99 6.5.2.5p3). It can therefore
// be assigned to, have its address taken, and be the operand of ++/--.
int main(void) {
	int r = ((int){3} = 5); // assign to a scalar compound literal -> 5
	int *p = &(int){9};     // take the address of a scalar compound literal
	*p += 1;                // mutate through the pointer -> 10
	int q = (int){7}++;     // post-increment a compound-literal lvalue -> q == 7
	return r + *p + q;      // 5 + 10 + 7 == 22
}
