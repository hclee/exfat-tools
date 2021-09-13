#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <time.h>

#include "version.h"
#include "exfat_ondisk.h"
#include "libexfat.h"

#define FSCK_PROG	"fsck.exfat"
#define MAX_FSCK_ARGS	32

/**
 * EFSCK_EXIT_FAILURE		Unknown or errors left
 * EFSCK_EXIT_VOLUME_DIRTY	VolumeDirty flag in boot sector
 * EFSCK_EXIT_USER_CANCEL	Killed by signal or device is removed
 */
#define EFSCK_EXIT_SUCCESS		0
#define EFSCK_EXIT_FAILURE		1
#define EFSCK_EXIT_SYNTAX_ERROR		2
#define EFSCK_EXIT_NOT_FAT_VOLUME	3
#define EFSCK_EXIT_RO_DEVICE		23
#define EFSCK_EXIT_VOLUME_DIRTY		100
#define EFSCK_EXIT_USER_CANCEL		160
#define EFSCK_EXIT_TIMEOUT		161

#define FSCK_EXIT_NO_ERRORS		0x00
#define FSCK_EXIT_CORRECTED		0x01
#define FSCK_EXIT_NEED_REBOOT		0x02
#define FSCK_EXIT_ERRORS_LEFT		0x04
#define FSCK_EXIT_OPERATION_ERROR	0x08
#define FSCK_EXIT_SYNTAX_ERROR		0x10
#define FSCK_EXIT_USER_CANCEL		0x20
#define FSCK_EXIT_LIBRARY_ERROR		0x80

#ifndef __unused
#define __unused	__attribute__((__unused__))
#endif

pid_t fsck_pid;

static void usage(char *name)
{
	printf("exfatprogs version : %s\n", WEBOS_EXFAT_PROGS_VERSION);
	fprintf(stderr, "Usage: %s\n", name);
	fprintf(stderr, "\t-h                     Show help\n");
	fprintf(stderr, "\t-V                     Show version\n");
	fprintf(stderr, "\t-a                     Exit if Volume flag is clean\n");
	fprintf(stderr, "\t-y                     Repair the filesystem without user interaction\n");
	fprintf(stderr, "\t-t seconds             Run with a time limit\n");
	fprintf(stderr, "This util just runs %s.\n", FSCK_PROG);
	exit(EFSCK_EXIT_SYNTAX_ERROR);
}

static int kill_fsck(void)
{
	kill(fsck_pid, SIGTERM);
	waitpid(fsck_pid, NULL, 0);
	return 0;
}

static void handle_timeout(int sig __unused, siginfo_t *si __unused,
			   void *u __unused)
{
	exfat_debug("timer is expired!\n");
}

static void handle_cancel_signals(int sig, siginfo_t *si __unused,
				  void *u __unused)
{
	exfat_err("killed by signal %d\n", sig);
	kill_fsck();
	exit(EFSCK_EXIT_USER_CANCEL);
}

static int setup_signal_handlers(unsigned long timeout_secs)
{
	struct sigaction sa;
	sigset_t sigmask;

	sigfillset(&sigmask);
	sigdelset(&sigmask, SIGCHLD);
	sigdelset(&sigmask, SIGALRM);
	sigdelset(&sigmask, SIGINT);
	sigdelset(&sigmask, SIGTERM);
	if (sigprocmask(SIG_SETMASK, &sigmask, NULL) != 0)
		exfat_err("sigprocmask failed: %s\n", strerror(errno));

	memset(&sa, 0, sizeof(sa));
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = handle_cancel_signals;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	if (timeout_secs) {
		sa.sa_sigaction = handle_timeout;
		if (sigaction(SIGALRM, &sa, NULL) != 0) {
			exfat_err("failed to set signal handler: %s\n",
				  strerror(errno));
			return -1;
		}

		alarm((unsigned int)timeout_secs);
	}
	return 0;
}

static int wait_for_fsck(int *exit_status)
{
	int wait_status;

	while (1) {
		if (waitpid(fsck_pid, &wait_status, 0) < 0) {
			/* timer is expired */
			if (errno == EINTR) {
				kill_fsck();
				*exit_status = FSCK_EXIT_USER_CANCEL;
				return -EINTR;
			} else {
				exfat_err("failed to waitpid: %s\n",
					  strerror(errno));
				kill_fsck();
				*exit_status = EFSCK_EXIT_FAILURE;
				return -EINVAL;
			}
		}
		if (WIFEXITED(wait_status)) {
			*exit_status = WEXITSTATUS(wait_status);
			return 0;
		}
	}
	return 0;
}

static int read_boot_sect(const char *device_file, char sect[], size_t len)
{
	int fd;
	ssize_t bytes;

	fd = open(device_file, O_RDONLY);
	if (fd < 0) {
		exfat_err("failed to open %s to check exfat volume: %s\n",
			  device_file, strerror(errno));
		return fd;
	}

	bytes = read(fd, sect, len);
	if (bytes != (ssize_t)len) {
		exfat_err("failed to read %s to check exfat volume\n",
			  device_file);
		close(fd);
		return -EIO;
	}

	close(fd);
	return 0;
}

static bool is_exfat_clean(const char *device_file)
{
	char sect[512];

	if (read_boot_sect(device_file, sect, sizeof(sect)) != 0)
		return false;

	if (sect[106] && 0x2)
		return false;
	return true;
}

static bool is_exfat_volume(const char *device_file)
{
	char sect[512];

	if (read_boot_sect(device_file, sect, sizeof(sect)) != 0)
		return false;

	if (memcmp(sect + 3, "EXFAT   ", 8) != 0)
		return false;
	return true;
}

int main(int argc, char *argv[])
{
	char *fsck_argv[MAX_FSCK_ARGS + 2] = {FSCK_PROG, };
	char *device_file;
	unsigned long timeout_secs = 0;
	bool version_only = false, exit_if_clean_volume = false;
	int fsck_status, exit_status = EFSCK_EXIT_SUCCESS;
	int i, k;

	print_level = EXFAT_ERROR;

	/* handle options */
	i = k = 1;
	while (i < argc) {
		if (k >= MAX_FSCK_ARGS)
			usage(argv[0]);
		if (strcmp(argv[i], "-V") == 0)
			version_only = true;
		else if (strcmp(argv[i], "-h") == 0)
			usage(argv[0]);
		else if (strcmp(argv[i], "-t") == 0) {
			char *endptr = NULL;

			if (i + 1 >= argc || argv[i + 1][0] == '-')
				usage(argv[0]);

			timeout_secs = strtoul(argv[++i], &endptr, 10);
			if (endptr && *endptr != '\0')
				usage(argv[0]);
		} else if (strcmp(argv[i], "-a") == 0) {
			exit_if_clean_volume = true;
			fsck_argv[k++] = "-y";
			fsck_argv[k++] = "-s";
		} else if (strcmp(argv[i], "-y") == 0) {
			fsck_argv[k++] = "-y";
			fsck_argv[k++] = "-s";
		} else if (strcmp(argv[i], "-") == 0) {
			usage(argv[0]);
		} else {
			fsck_argv[k++] = argv[i];
		}
		i++;
	}
	device_file = fsck_argv[k-1];
	fsck_argv[k] = NULL;

	if (version_only)
		usage(argv[0]);

	if (exit_if_clean_volume && is_exfat_clean(device_file))
		exit(EFSCK_EXIT_SUCCESS);

	/* run fsck */
	fsck_pid = fork();
	if (fsck_pid < 0) {
		exfat_err("failed to fork for %s: %s\n", FSCK_PROG,
			  strerror(errno));
		exit(EFSCK_EXIT_FAILURE);
	} else if (fsck_pid == 0) {
		execvp(FSCK_PROG, fsck_argv);
		exfat_err("failed to exec %s: %s\n", FSCK_PROG,
			  strerror(errno));
		exit(EFSCK_EXIT_FAILURE);
	}

	if (setup_signal_handlers(timeout_secs) != 0) {
		kill_fsck();
		exit_status = EFSCK_EXIT_FAILURE;
		goto out;
	}

	wait_for_fsck(&fsck_status);

	/* handle exit status */
	if (fsck_status == FSCK_EXIT_OPERATION_ERROR) {
		struct stat st;

		if (stat(device_file, &st) != 0) {
			if (errno == ENOENT)
				exit_status = EFSCK_EXIT_USER_CANCEL;
			else
				exit_status = EFSCK_EXIT_FAILURE;
			goto out;
		}

		if (st.st_mode & S_IWUSR)
			exit_status = EFSCK_EXIT_FAILURE;
		else
			exit_status = EFSCK_EXIT_RO_DEVICE;
	} else if (fsck_status == FSCK_EXIT_USER_CANCEL) {
		exfat_debug("timer is expired. %s is killed\n", FSCK_PROG);
		exit_status = EFSCK_EXIT_TIMEOUT;
	} else if (fsck_status == FSCK_EXIT_SYNTAX_ERROR) {
		usage(argv[0]);
	} else if (fsck_status == FSCK_EXIT_ERRORS_LEFT) {
		if (is_exfat_volume(device_file)) {
			exfat_err("there are still errors after fsck\n");
			exit_status = EFSCK_EXIT_FAILURE;
		}  else
			exit_status = EFSCK_EXIT_NOT_FAT_VOLUME;
	}  else if (fsck_status != FSCK_EXIT_NO_ERRORS &&
		   fsck_status != FSCK_EXIT_CORRECTED) {
		exit_status = EFSCK_EXIT_FAILURE;
	}
out:
	exit(exit_status);
}
