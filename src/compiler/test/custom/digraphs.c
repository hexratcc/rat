// expect: 9
// C99 digraphs (6.4.6): <: :> => [ ]   <% %> => { }   %: => #
%:define SQ(x) ((x) * (x))
int main(void) <%
	int a<:3:> = <% 1, 2, 3 %>; // int a[3] = { 1, 2, 3 };
	return a<:0:> + a<:1:> + a<:2:> + SQ(0) + 3; // 1+2+3+0+3 = 9
%>
