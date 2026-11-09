#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "Usage: %s <cmd> [args...]\n", argv[0]);
		return 1;
	}

	int master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0)
	{
		perror("posix_openpt");
		return 1;
	}
	if (grantpt(master)  < 0)
	{
		perror("grantpt");
		return 1;
	}
	if (unlockpt(master) < 0)
	{
		perror("unlockpt");
		return 1;
	}
	char *slave_name = ptsname(master);
	if (!slave_name)
	{
		perror("ptsname");
		return 1;
	}

	pid_t pid = fork();
	if (pid < 0)
	{
		perror("fork");
		return 1;
	}

	if (pid == 0)
	{
		// child: new session, slave PTY as controlling terminal
		setsid();
		int slave = open(slave_name, O_RDWR);
		if (slave < 0) _exit(2);
		ioctl(slave, TIOCSCTTY, 0);
		dup2(slave, 0);
		dup2(slave, 1);
		dup2(slave, 2);
		if (slave > 2) close(slave);
		close(master);
		setenv("TERM", "xterm", 0); // mksh enables line editing if TERM set
		setenv("PS1", "# ", 0);
		execvp(argv[1], argv + 1);
		_exit(127);
	}

	// parent: relay stdin<->master, master->stdout
	signal(SIGCHLD, SIG_IGN);
	fd_set fds;
	char buf[4096];
	int maxfd = master + 1;
	for (;;)
	{
		FD_ZERO(&fds);
		FD_SET(0, &fds);
		FD_SET(master, &fds);
		int r = select(maxfd, &fds, NULL, NULL, NULL);
		if (r < 0)
		{
			if (errno == EINTR) continue;
			break;
		}
		if (FD_ISSET(0, &fds))
		{
			ssize_t n = read(0, buf, sizeof(buf));
			if (n <= 0) break;
			write(master, buf, n);
		}
		if (FD_ISSET(master, &fds))
		{
			ssize_t n = read(master, buf, sizeof(buf));
			if (n <= 0) break;
			write(1, buf, n);
		}
	}
	return 0;
}
