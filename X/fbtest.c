#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

int main() {
	int fb = open("/dev/graphics/fb0", O_RDWR);
	if (fb < 0)
	{
		perror("open");
		return 1;
	}

	struct fb_var_screeninfo vinfo;
	struct fb_fix_screeninfo finfo;
	ioctl(fb, FBIOGET_VSCREENINFO, &vinfo);
	ioctl(fb, FBIOGET_FSCREENINFO, &finfo);

	size_t screensize = finfo.smem_len;
	char *fbp = mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
	if (fbp == MAP_FAILED)
	{
		perror("mmap");
		return 1;
	}

	memset(fbp, 0xff, screensize);
	ioctl(fb, FBIOPAN_DISPLAY, &vinfo);

	while (1) pause();

	return 0;
}