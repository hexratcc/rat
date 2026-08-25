#ifndef _RATCC_PWD_H
#define _RATCC_PWD_H

#if defined(_WIN32)
#error "<pwd.h> is not available when targeting windows"
#endif

#include <sys/types.h>

struct passwd {
	char* pw_name;
	char* pw_passwd;
	uid_t pw_uid;
	gid_t pw_gid;
	char* pw_gecos;
	char* pw_dir;
	char* pw_shell;
};

struct passwd* getpwuid(uid_t uid);
struct passwd* getpwnam(const char* name);
int getpwuid_r(uid_t uid, struct passwd* pw, char* buf, size_t size, struct passwd** out);
int getpwnam_r(const char* name, struct passwd* pw, char* buf, size_t size, struct passwd** out);
void endpwent(void);

#endif
