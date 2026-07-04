// expect: 0
typedef unsigned char u8;
struct SS { u8 a[3], b; };
struct SS sinit16[] = { { 1 }, 2 };
int main() {
  if (sizeof(sinit16)/sizeof(sinit16[0]) != 2) return 10;
  if (sinit16[0].a[0] != 1 || sinit16[0].a[1] != 0 || sinit16[0].b != 0) return 1;
  if (sinit16[1].a[0] != 2 || sinit16[1].a[1] != 0 || sinit16[1].b != 0) return 2;
  return 0;
}
