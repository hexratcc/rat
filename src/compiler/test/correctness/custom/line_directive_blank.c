// expect: 0
// blank lines after #line still count
#line 100

int main(void) { return __LINE__ - 101; }
