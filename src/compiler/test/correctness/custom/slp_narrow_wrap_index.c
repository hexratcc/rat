// expect: 0
// int indices near INT_MIN whose folded constants would wrap: four adjacent lanes must stay
// adjacent for the address analysis, not split into two windows half the address space apart
float F[16], G[16];

__attribute__((noinline)) void k(void) {
	for(int i = -2147483643; i <= -2147483637; i += 4) {
		F[(i + 0) - (-2147483643) + 3] = G[(i + 0) - (-2147483643) + 3] * 2.0f;
		F[(i + 1) - (-2147483643) + 3] = G[(i + 1) - (-2147483643) + 3] * 2.0f;
		F[(i + 2) - (-2147483643) + 3] = G[(i + 2) - (-2147483643) + 3] * 2.0f;
		F[(i + 3) - (-2147483643) + 3] = G[(i + 3) - (-2147483643) + 3] * 2.0f;
	}
}

int main(void) {
	for(int i = 0; i < 16; i++)
		G[i] = (float)i;
	k();
	for(int i = 3; i < 11; i++)
		if(F[i] != 2.0f * i)
			return 1;
	return 0;
}
