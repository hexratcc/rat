// expect: 2
// 3<=3 is 1, 4>=5 is 0, 1==1 is 1, 2!=2 is 0; sum == 2.
int main(void) {
	return (3 <= 3) + (4 >= 5) + (1 == 1) + (2 != 2);
}
