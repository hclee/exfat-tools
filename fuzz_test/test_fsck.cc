
#include <cstdint>
#include "types.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

extern "C" {
#include "exfat_ondisk.h"
#include "libexfat.h"
#include "exfat_dir.h"
#include "fsck.h"
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	const char *filename = "/tmp/exfat_test_file";
#if 0
#else
	char filepath[PATH_MAX];
#endif
	int fd, ret;
	struct fsck_user_input ui;

#if 0
	fd = open(filename, O_RDWR|O_CREAT|O_TRUNC);
#else
	fd = syscall(SYS_memfd_create, filename, 0);
#endif
	if (fd < 0) {
		perror("open");
		return 1;
	}

	if (write(fd, data, size) != (ssize_t)size) {
		perror("write");
		close(fd);
		return 1;
	}

	memset(&ui, 0, sizeof(ui));
#if 0
	ui.ei.dev_name = filename;
#else
	snprintf(filepath, sizeof(filepath), "/proc/self/fd/%d", fd);
	ui.ei.dev_name = filepath;
#endif
	ui.ei.writeable = true;
	ui.options = (enum fsck_ui_options)(FSCK_OPTS_REPAIR_YES | FSCK_OPTS_REPAIR_WRITE);

	ret = exfat_fsck_main(&ui);

	close(fd);
	return ret;
}
