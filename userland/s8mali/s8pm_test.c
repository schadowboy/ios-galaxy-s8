/*
 * s8pm_test: does the real restart / power off work (patch 0152, boot-arg s8noreset)? Removable test scaffolding.
 * If /var/mobile/s8pm_mode exists ("restart" or "halt"), it is deleted FIRST (so a failing test cannot loop), the file system is synced, and
 * after a delay the reboot() system call is made; the log is /var/mobile/s8pm_test.log.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

extern int reboot(int howto);

int
main(void)
{
	FILE *lf = fopen("/var/mobile/s8pm_test.log", "a");
	FILE *m = fopen("/var/mobile/s8pm_mode", "r");
	char mode[32] = "";
	int howto, r;

	if (lf == NULL) {
		return 1;
	}
	if (m == NULL) {
		fprintf(lf, "S8PM: no mode file, nothing to do\n");
		fclose(lf);
		return 0;
	}
	if (fgets(mode, sizeof(mode), m) == NULL) {
		mode[0] = 0;
	}
	fclose(m);
	fprintf(lf, "S8PM: start pid %d uid %d mode '%s'\n", (int)getpid(), (int)getuid(), mode);
	fflush(lf);
	unlink("/var/mobile/s8pm_mode");
	sync();
	sleep(40);
	howto = strncmp(mode, "halt", 4) == 0 ? 0x08 /* RB_HALT */ : 0 /* RB_AUTOBOOT */;
	fprintf(lf, "S8PM: calling reboot(0x%x) now\n", howto);
	fflush(lf);
	fsync(fileno(lf));
	sync();
	usleep(300000);
	r = reboot(howto);
	fprintf(lf, "S8PM: reboot returned %d errno %d\n", r, errno);
	fclose(lf);
	return 0;
}
