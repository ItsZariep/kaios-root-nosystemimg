#define _GNU_SOURCE
#include <termios.h>
#include <unistd.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "Usage: %s <cmd> [args...]\n", argv[0]);
		return 1;
	}
	struct termios t;
	if (tcgetattr(0, &t) == 0)
	{
		t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
		t.c_iflag &= ~(IXON | ICRNL | INLCR | ISTRIP);
		t.c_oflag &= ~OPOST;
		t.c_cc[VMIN] = 1;
		t.c_cc[VTIME] = 0;
		tcsetattr(0, TCSANOW, &t);
	}
	execvp(argv[1], argv + 1);
	perror("execvp");
	return 127;
}
