
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include "exfat_ondisk.h"
#include "libexfat.h"
#include "exfat_dir.h"
#include "fsck.h"

int main(int argc, char *argv[])
{
	const char *filename = argv[1];
	int ret;
	struct fsck_user_input ui;

	memset(&ui, 0, sizeof(ui));
	ui.ei.dev_name = filename;
	ui.ei.writeable = true;
	ui.options = (enum fsck_ui_options)(FSCK_OPTS_REPAIR_YES | FSCK_OPTS_REPAIR_WRITE);

	ret = exfat_fsck_main(&ui);
	return ret;
}
