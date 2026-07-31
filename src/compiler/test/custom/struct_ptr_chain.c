// expect: 42
// Linked structure traversal via pointers.
struct Node { int val; struct Node *next; };

int main(void) {
	struct Node a;
	struct Node b;
	a.val = 2;
	a.next = &b;
	b.val = 40;
	b.next = &a;
	return a.next->val + a.val;
}
