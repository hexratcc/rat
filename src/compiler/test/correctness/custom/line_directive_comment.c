// expect: 0
// a block comment closing a #line keeps the lines after it
#line 10 /* a
 b */
int x = __LINE__;
int main(void) { return x - 10; }
