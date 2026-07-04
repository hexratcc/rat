// expect: 3
// Enumerators auto-increment from zero (RED=0, GREEN=1, BLUE=2).
enum Color { RED, GREEN, BLUE };

int main(void) {
	return GREEN + BLUE;
}
