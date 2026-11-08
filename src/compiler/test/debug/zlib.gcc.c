struct Box {
	int a;
	int b;
};

int scale = 3;

int unbox(struct Box* box) {
	int sum = box->a + box->b;
	return sum * scale;
}
