// expect: 0
struct SEA { int i; int j; int k; int l; };
struct SEB { struct SEA a; int r[1]; };
struct SEA gx = { .j = 7 };
struct SEB gb = { .a.j = 5, .a.l = 9 };
int arr2[3][3] = { [1][2] = 4, [0][0] = 1 };
struct Pt { int x, y; };
struct Pt pts[3] = { [2].y = 8, [0].x = 3 };
int main() {
  struct SEB b = { .a.j = 5 };
  if (b.a.i != 0 || b.a.j != 5 || b.a.k != 0 || b.a.l != 0) return 1;
  if (gb.a.j != 5 || gb.a.l != 9 || gb.a.i != 0) return 2;
  if (arr2[1][2] != 4 || arr2[0][0] != 1 || arr2[2][2] != 0) return 3;
  if (pts[2].y != 8 || pts[0].x != 3 || pts[1].x != 0) return 4;
  if (gx.j != 7 || gx.i != 0) return 5;
  return 0;
}
