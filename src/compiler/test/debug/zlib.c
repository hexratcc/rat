struct Box {
	int a;
	int b;
};

int unbox(struct Box* box);

int main(void) {
	struct Box box = {4, 5};
	return unbox(&box) & 1;
}
