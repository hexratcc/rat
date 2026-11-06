// expect: 0
// a #line continued by a splice still numbers the line after it
#line 100 \

int x = __LINE__;
int main(void) { return x - 100; }
