// expect: 0
// skip-target: windows

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

int main(void) {
	struct stat st;
	if (stat("/dev/null", &st) != 0)
		return 1;
	if (!S_ISCHR(st.st_mode))
		return 2;
	if (stat("/", &st) != 0 || !S_ISDIR(st.st_mode) || st.st_nlink == 0)
		return 3;
	if (st.st_mtime <= 0 || st.st_mtim.tv_sec != st.st_mtime)
		return 4;
	if (access("/", R_OK | X_OK) != 0)
		return 5;

	int fd = open("/dev/null", O_RDONLY);
	if (fd < 0)
		return 6;
	if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode))
		return 7;
	if ((fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDONLY)
		return 8;
	if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || fcntl(fd, F_GETFD) != FD_CLOEXEC)
		return 9;
	if (close(fd) != 0)
		return 10;

	// readdir writes through a 280-byte glibc dirent; d_name must land at 19
	DIR* d = opendir("/");
	if (!d)
		return 11;
	int sawDot = 0;
	struct dirent* e;
	while ((e = readdir(d)) != 0)
		if (strcmp(e->d_name, ".") == 0 && (e->d_type == DT_DIR || e->d_type == DT_UNKNOWN))
			sawDot = 1;
	if (closedir(d) != 0 || !sawDot)
		return 12;

	if (sysconf(_SC_PAGESIZE) < 4096)
		return 13;

	// the kernel fills 144 bytes here; a short struct would corrupt the stack
	struct rusage ru;
	if (getrusage(RUSAGE_SELF, &ru) != 0 || ru.ru_maxrss <= 0)
		return 14;

	struct timeval tv;
	if (gettimeofday(&tv, 0) != 0 || tv.tv_sec < 1600000000 || tv.tv_usec >= 1000000)
		return 15;
	return 0;
}
