// expect: 1

// predefined macro: C99 sets __STDC_VERSION__ to 199901L.
int main(void) {
#if __STDC_VERSION__ >= 199901L
	return 1;
#else
	return 0;
#endif
}
