#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <time.h>

#include "exfat_ondisk.h"
#include "libexfat.h"

#define FSCK_PROG	"fsck.exfat"
#define MAX_FSCK_ARGS	32

#define EXIT_FORK		2
#define EXIT_RO_DEVICE		23
#define EXIT_DEVICE_REMOVED	160
#define EXIT_TIMEOUT		161

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
	fprintf(stderr, "Usage: %s\n", name);
	fprintf(stderr, "\t-h                     Show help\n");
	fprintf(stderr, "\t-V                     Show version\n");
	fprintf(stderr, "\t-t seconds             Run with a time limit\n");
	fprintf(stderr, "\tAnd %s -h. This util just runs %s.\n",
		FSCK_PROG, FSCK_PROG);
	exit(EXIT_FAILURE);
}

static void handle_timeout(int sig __unused, siginfo_t *si __unused,
			   void *u __unused)
{
	exfat_debug("timer is expired!\n");
}

static int setup_timer(unsigned long timeout_secs)
{
	struct sigaction sa;
	sigset_t sigmask;

	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = handle_timeout;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGALRM, &sa, NULL) != 0) {
		exfat_err("failed to set signal handler: %s\n",
			  strerror(errno));
		return -1;
	}

	sigfillset(&sigmask);
	sigdelset(&sigmask, SIGCHLD);
	sigdelset(&sigmask, SIGALRM);
	if (sigprocmask(SIG_BLOCK, &sigmask, NULL) != 0)
		exfat_err("sigprocmask failed: %s\n", strerror(errno));

	alarm((unsigned int)timeout_secs);
	return 0;
}

static int kill_fsck(void)
{
	kill(fsck_pid, SIGTERM);
	waitpid(fsck_pid, NULL, 0);
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
				*exit_status = EXIT_FAILURE;
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

int main(int argc, char *argv[])
{
	char *fsck_argv[MAX_FSCK_ARGS + 1] = {FSCK_PROG, };
	char *device_file;
	unsigned long timeout_secs = 0;
	bool version_only = false, need_writeable = true;
	int fsck_status, exit_status = 0;
	int i, k;

	print_level = EXFAT_ERROR;

	/* handle options */
	i = k = 1;
	while (i < argc) {
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
		} else {
			if (k >= MAX_FSCK_ARGS)
				usage(argv[0]);
			if (strcmp(argv[i], "-n") == 0 ||
			    strcmp(argv[i], "--repair-no") == 0)
				need_writeable = false;
			fsck_argv[k++] = argv[i];
		}
		i++;
	}
	device_file = fsck_argv[k-1];
	fsck_argv[k] = NULL;

	if (version_only) {
		show_version();
		usage(argv[0]);
	}

	/* run fsck */
	fsck_pid = fork();
	if (fsck_pid < 0) {
		exfat_err("failed to fork for %s: %s\n", FSCK_PROG,
			  strerror(errno));
		exit(EXIT_FORK);
	} else if (fsck_pid == 0) {
		execvp(FSCK_PROG, fsck_argv);
		exfat_err("failed to exec %s: %s\n", FSCK_PROG,
			  strerror(errno));
		exit(EXIT_FORK);
	}

	if (timeout_secs && setup_timer(timeout_secs) != 0) {
		kill_fsck();
		exit_status = EXIT_FAILURE;
		goto out;
	}

	wait_for_fsck(&fsck_status);

	/* handle exit status */
	if (fsck_status == FSCK_EXIT_OPERATION_ERROR) {
		struct stat st;

		if (stat(device_file, &st) != 0) {
			if (errno == ENOENT)
				exit_status = EXIT_DEVICE_REMOVED;
			else
				exit_status = EXIT_FAILURE;
			goto out;
		}

		if (need_writeable && ~(st.st_mode & S_IWUSR))
			exit_status = EXIT_RO_DEVICE;
	} else if (fsck_status == FSCK_EXIT_USER_CANCEL) {
		exfat_debug("timer is expired. %s is killed\n", FSCK_PROG);
		exit_status = EXIT_TIMEOUT;
	} else if (fsck_status == FSCK_EXIT_SYNTAX_ERROR) {
		usage(argv[0]);
	} else if (fsck_status != FSCK_EXIT_NO_ERRORS &&
		   fsck_status != FSCK_EXIT_CORRECTED) {
		exit_status = EXIT_FAILURE;
	}
out:
	exit(exit_status);
}
