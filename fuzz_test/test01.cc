
#include <cstdint>
#include "types.h"

#include <sys/types.h>
#include <sys/stat.h>
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

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	const char *filename = "/tmp/exfat_test_file";
	int fd;
	struct fsck_user_input ui;

	fd = open(filename, O_RDWR|O_CREAT|O_TRUNC);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	if (write(fd, data, size) != (ssize_t)size) {
		perror("write");
		close(fd);
		return 1;
	}
	close(fd);

	memset(&ui, 0, sizeof(ui));
	ui.ei.dev_name = filename;
	ui.ei.writeable = false;
	ui.options = FSCK_OPTS_REPAIR_NO;

	return exfat_fsck_main(&ui);
}
