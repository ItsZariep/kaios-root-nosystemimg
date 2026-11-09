#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/select.h>

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "Usage: %s <socket_path>\n", argv[0]);
		return 1;
	}

	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0)
	{
		perror("socket");
		return 1;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, argv[1], sizeof(addr.sun_path) - 1);
	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		perror("connect");
		return 1;
	}

	fprintf(stderr, "[connected to %s]\n", argv[1]);
	fd_set fds;
	char buf[4096];

	while (1)
	{
		FD_ZERO(&fds);
		FD_SET(0, &fds);
		FD_SET(s, &fds);
		if (select(s + 1, &fds, NULL, NULL, NULL) < 0) break;
		if (FD_ISSET(0, &fds))
		{
			ssize_t n = read(0, buf, sizeof(buf));
			if (n <= 0) break;
			write(s, buf, n);
		}
		if (FD_ISSET(s, &fds))
		{
			ssize_t n = read(s, buf, sizeof(buf));
			if (n <= 0) break;
			write(1, buf, n);
		}
	}
	close(s);
	return 0;
}
