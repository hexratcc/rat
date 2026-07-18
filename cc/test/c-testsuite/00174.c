// expect: 0
// output: oracle
//| 69.120003
//| 69.120000
//| -44.440000
//| 700.665200
//| 0.217330
//| 1 1 0 0 0 1
//| 0 1 1 1 0 0
//| 0 0 0 1 1 1
//| 69.120003
//| -44.439999
//| 700.665222
//| 0.217330
//| 12.340000
//| -12.340000
//| 2.000000
//| 0.909297
#include <stdio.h>
#include <math.h>

int main()
{
   // variables
   float a = 12.34 + 56.78;
   printf("%f\n", a);

   // infix operators
   printf("%f\n", 12.34 + 56.78);
   printf("%f\n", 12.34 - 56.78);
   printf("%f\n", 12.34 * 56.78);
   printf("%f\n", 12.34 / 56.78);

   // comparison operators
   printf("%d %d %d %d %d %d\n", 12.34 < 56.78, 12.34 <= 56.78, 12.34 == 56.78, 12.34 >= 56.78, 12.34 > 56.78, 12.34 != 56.78);
   printf("%d %d %d %d %d %d\n", 12.34 < 12.34, 12.34 <= 12.34, 12.34 == 12.34, 12.34 >= 12.34, 12.34 > 12.34, 12.34 != 12.34);
   printf("%d %d %d %d %d %d\n", 56.78 < 12.34, 56.78 <= 12.34, 56.78 == 12.34, 56.78 >= 12.34, 56.78 > 12.34, 56.78 != 12.34);

   // assignment operators
   a = 12.34;
   a += 56.78;
   printf("%f\n", a);

   a = 12.34;
   a -= 56.78;
   printf("%f\n", a);

   a = 12.34;
   a *= 56.78;
   printf("%f\n", a);

   a = 12.34;
   a /= 56.78;
   printf("%f\n", a);

   // prefix operators
   printf("%f\n", +12.34);
   printf("%f\n", -12.34);

   // type coercion
   a = 2;
   printf("%f\n", a);
   printf("%f\n", sin(2));

   return 0;
}

/* vim: set expandtab ts=4 sw=3 sts=3 tw=80 :*/
