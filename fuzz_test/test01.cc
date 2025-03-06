
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

int test_read_boot_sect(const char *filename)
{
	struct exfat_user_input ui;
	struct exfat_blk_dev bd;
	struct pbr *bs;
	int ret;

	memset(&ui, 0, sizeof(ui));
	memset(&bd, 0, sizeof(bd));

	ui.dev_name = filename;
	ret = exfat_get_blk_dev_info(&ui, &bd);
	if (ret < 0) {
		printf("ERROR: exfat_get_blk_dev_info\n");
		return ret;
	}

	ret = exfat_boot_region_check(&bd, &bs, false);
	close(bd.dev_fd);
	return ret;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	const char *filename = "/tmp/exfat_test_file";
	int fd;

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

	test_read_boot_sect(filename);
	return 0;
}
