// expect: 0
// pointer stores pack as integer lanes: a {next, prev} pair copies with one 16-byte load and store,
// also when the source is the destination shifted by one pair
struct node {
	struct node* next;
	struct node* prev;
};

struct node N[4];

__attribute__((noinline)) void link(struct node* restrict d, const struct node* restrict s) {
	d->next = s->next;
	d->prev = s->prev;
}

__attribute__((noinline)) void shift(struct node* n) {
	n[0].next = n[1].next;
	n[0].prev = n[1].prev;
	n[1].next = n[2].next;
	n[1].prev = n[2].prev;
}

int main(void) {
	for(int i = 0; i < 4; i++) {
		N[i].next = &N[(i + 1) % 4];
		N[i].prev = &N[(i + 3) % 4];
	}
	link(&N[0], &N[2]);
	if(N[0].next != &N[3] || N[0].prev != &N[1])
		return 1;
	shift(N);
	if(N[0].next != &N[2] || N[0].prev != &N[0] || N[1].next != &N[3] || N[1].prev != &N[1])
		return 2;
	return 0;
}
