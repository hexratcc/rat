// expect: 0

// the pointee of a const pointer does not keep the const of the pointer

int value = 5;
int* const constPtr = &value;

int main(void) {
	__typeof__(*constPtr)* other;
	other = &value;
	return *other - 5 + _Generic(&*constPtr, int*: 0, default: 1);
}
