#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h> // <-- Required for va_start, va_arg, va_end, va_list

static int (*real_close)(int) = NULL;
static int fb_fd = -1;

static void *pan_thread(void *arg)
{
	struct fb_var_screeninfo vinfo;
	while (1)
	{
		if (fb_fd >= 0)
		{
			if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0)
			{
				ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo);
			}
		}
		usleep(100000);
	}
	return NULL;
}

int open(const char *pathname, int flags, ...)
{
	static int (*real_open)(const char *, int, ...) = NULL;
	if (!real_open) real_open = dlsym(RTLD_NEXT, "open");

	mode_t mode = 0;
	if (flags & O_CREAT)
	{
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	int fd = real_open(pathname, flags, mode);

	if (fd >= 0 && pathname && strstr(pathname, "/dev/graphics/fb0"))
	{
		fprintf(stderr, "[fbhack] intercepted open of %s -> fd %d\n", pathname, fd);
		fb_fd = fd;
		static pthread_t t;
		static int started = 0;
		if (!started)
		{
			pthread_create(&t, NULL, pan_thread, NULL);
			started = 1;
		}
	}
	return fd;
}

int close(int fd)
{
	if (!real_close) real_close = dlsym(RTLD_NEXT, "close");
	if (fd == fb_fd)
	{
		fprintf(stderr, "[fbhack] blocked close() of fb fd %d\n", fd);
		return 0; // pretend it succeeded, keep it open for real
	}
	return real_close(fd);
}