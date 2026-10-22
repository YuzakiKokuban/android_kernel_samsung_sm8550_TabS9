// SPDX-License-Identifier: GPL-2.0
/* Run only on a dedicated test directory, never on an application directory. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "FAIL: %s (errno=%d: %s)\n", what, errno,
			strerror(errno));
		exit(1);
	}
}

static void get_attr(int fd, struct fsxattr *attr)
{
	memset(attr, 0, sizeof(*attr));
	check(ioctl(fd, FS_IOC_FSGETXATTR, attr) == 0, "GETXATTR");
}

static void race_remount(const char *mountpoint, int failed)
{
	int root = open(mountpoint, O_RDONLY | O_DIRECTORY);
	int fd, status;
	pid_t pid;

	check(root >= 0, "open race mount");
	check(mkdirat(root, failed ? "race-failed" : "race", 0700) == 0,
	      "create race directory");
	fd = openat(root, failed ? "race-failed" : "race", O_RDONLY | O_DIRECTORY);
	check(fd >= 0, "open race directory");
	pid = fork();
	check(pid >= 0, "fork remount race");
	if (!pid) {
		int i;

		for (i = 0; i < 20000; i++) {
			struct fsxattr attr;
			int ret;

			get_attr(fd, &attr);
			attr.fsx_projid = 12345;
			attr.fsx_xflags |= FS_XFLAG_PROJINHERIT;
			ret = ioctl(fd, FS_IOC_FSSETXATTR, &attr);
			if (!ret) {
				check(!failed, "failed remount cannot enable fallback");
				get_attr(fd, &attr);
				check(!(attr.fsx_xflags & FS_XFLAG_PROJINHERIT),
				      "successful fallback cannot persist inheritance");
			} else {
				check(errno == EOPNOTSUPP, "race preserves strict error");
			}
		}
		_exit(0);
	}
	for (int i = 0; i < 1000; i++) {
		if (failed) {
			errno = 0;
			check(mount(NULL, mountpoint, NULL, MS_REMOUNT,
				    "legacy_project_quota,invalid_option") == -1 &&
			      errno == EINVAL, "invalid remount rejected");
		} else {
			check(mount(NULL, mountpoint, NULL, MS_REMOUNT,
				    "legacy_project_quota") == 0, "race enable");
			check(mount(NULL, mountpoint, NULL, MS_REMOUNT,
				    "nolegacy_project_quota") == 0, "race disable");
		}
	}
	check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
	      WEXITSTATUS(status) == 0, "remount race result");
	close(fd);
	close(root);
	puts("PASS: remount race");
}

int main(int argc, char **argv)
{
	struct fsxattr attr;
	int fd, flags, ret, status;
	int compat, native, readonly, verify;
	pid_t pid;

	if (argc != 3) {
		fprintf(stderr, "usage: %s TEST_DIR MODE (see run.sh)\n",
			argv[0]);
		return 2;
	}
	if (!strcmp(argv[2], "race") || !strcmp(argv[2], "race-failed")) {
		race_remount(argv[1], !strcmp(argv[2], "race-failed"));
		return 0;
	}
	compat = !strcmp(argv[2], "compat");
	native = !strcmp(argv[2], "native");
	readonly = !strcmp(argv[2], "readonly");
	verify = !strncmp(argv[2], "verify-", 7);
	check(compat || native || readonly || verify ||
	      !strcmp(argv[2], "strict"), "known test mode");
	fd = open(argv[1], O_RDONLY | O_DIRECTORY);
	check(fd >= 0, "open test directory");
	get_attr(fd, &attr);
	if (verify) {
		int real = !strcmp(argv[2], "verify-native");

		check(real || !strcmp(argv[2], "verify-compat"), "verify mode");
		check(attr.fsx_projid == (real ? 12345U : 0U), "persisted project ID");
		check(!!(attr.fsx_xflags & FS_XFLAG_PROJINHERIT) == real,
		      "persisted inheritance");
		puts("PASS: persisted project attributes");
		return 0;
	}
	attr.fsx_projid = 12345;
	errno = 0;
	ret = ioctl(fd, FS_IOC_FSSETXATTR, &attr);
	if (readonly) {
		check(ret == -1 && errno == EROFS, "read-only project ID rejected");
		check(ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0, "GETFLAGS");
		flags |= FS_PROJINHERIT_FL;
		errno = 0;
		check(ioctl(fd, FS_IOC_SETFLAGS, &flags) == -1 && errno == EROFS,
		      "read-only inheritance rejected");
		puts("PASS: read-only requests rejected");
		return 0;
	}
	if (!compat && !native) {
		check(ret == -1 && errno == EOPNOTSUPP, "strict legacy project ID rejected");
		puts("PASS: strict legacy behavior");
		return 0;
	}
	check(ret == 0, "SETXATTR project ID accepted");
	get_attr(fd, &attr);
	check(attr.fsx_projid == (native ? 12345U : 0U), "honest project ID readback");
	/* Images used here lack compression: an unrelated error must survive. */
	check(ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0, "GETFLAGS for invalid flags");
	flags |= FS_COMPR_FL;
	errno = 0;
	check(ioctl(fd, FS_IOC_SETFLAGS, &flags) == -1 && errno == EOPNOTSUPP,
	      "unsupported compression still rejected");
	check(ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0, "GETFLAGS");
	flags |= FS_PROJINHERIT_FL | FS_SYNC_FL;
	check(ioctl(fd, FS_IOC_SETFLAGS, &flags) == 0, "SETFLAGS inheritance");
	check(ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0, "GETFLAGS after update");
	check(!!(flags & FS_PROJINHERIT_FL) == native, "honest inheritance readback");
	check(flags & FS_SYNC_FL, "unrelated sync flag preserved");
	get_attr(fd, &attr);
	attr.fsx_projid = 12345;
	attr.fsx_xflags |= FS_XFLAG_PROJINHERIT;
	check(ioctl(fd, FS_IOC_FSSETXATTR, &attr) == 0, "combined SETXATTR");
	get_attr(fd, &attr);
	check(attr.fsx_projid == (native ? 12345U : 0U), "combined project ID readback");
	check(!!(attr.fsx_xflags & FS_XFLAG_PROJINHERIT) == native,
	      "combined inheritance readback");
	check(mkdirat(fd, "child", 0777) == 0, "create child");
	{
		int child = openat(fd, "child", O_RDONLY | O_DIRECTORY);

		check(child >= 0, "open child");
		get_attr(child, &attr);
		check(attr.fsx_projid == (native ? 12345U : 0U), "child project ID");
		close(child);
	}
	/* A directory owner without CAP_SYS_ADMIN must not gain the bypass. */
	if (compat) {
		int regular = openat(fd, "regular", O_CREAT | O_RDWR, 0600);

		check(regular >= 0, "create regular file");
		get_attr(regular, &attr);
		attr.fsx_projid = 12345;
		errno = 0;
		check(ioctl(regular, FS_IOC_FSSETXATTR, &attr) == -1 &&
		      errno == EOPNOTSUPP, "regular files cannot use fallback");
		close(regular);
		pid = fork();
		check(pid >= 0, "fork non-owner test");
		if (!pid) {
			check(setgid(65534) == 0 && setuid(65534) == 0,
			      "drop non-owner privileges");
			get_attr(fd, &attr);
			attr.fsx_projid = 12345;
			errno = 0;
			check(ioctl(fd, FS_IOC_FSSETXATTR, &attr) == -1 &&
			      errno == EPERM, "non-owner rejected by VFS");
			_exit(0);
		}
		check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
		      WEXITSTATUS(status) == 0, "non-owner test");
		check(fchown(fd, 65534, 65534) == 0, "prepare unprivileged owner");
		pid = fork();
		check(pid >= 0, "fork unprivileged test");
		if (!pid) {
			check(setgid(65534) == 0 && setuid(65534) == 0, "drop privileges");
			get_attr(fd, &attr);
			attr.fsx_projid = 12345;
			errno = 0;
			check(ioctl(fd, FS_IOC_FSSETXATTR, &attr) == -1 &&
			      errno == EOPNOTSUPP, "unprivileged project ID rejected");
			check(ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0, "owner GETFLAGS");
			flags |= FS_PROJINHERIT_FL;
			errno = 0;
			check(ioctl(fd, FS_IOC_SETFLAGS, &flags) == -1 && errno == EPERM,
			      "unprivileged inheritance rejected");
			_exit(0);
		}
		check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
		      WEXITSTATUS(status) == 0, "unprivileged tests");
		check(fchown(fd, 0, 0) == 0, "restore owner");
	}
	close(fd);
	printf("PASS: %s project attributes, inheritance and permissions\n", argv[2]);
	return 0;
}
