// expect: 20
// Enum constants used as switch case labels.
enum State { IDLE, RUNNING, DONE };

int main(void) {
	enum State s = RUNNING;
	int r = 0;
	switch (s) {
	case IDLE:
		r = 10;
		break;
	case RUNNING:
		r = 20;
		break;
	case DONE:
		r = 30;
		break;
	}
	return r;
}
