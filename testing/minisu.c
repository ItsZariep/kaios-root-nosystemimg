#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	if (setgid(0) || setuid(0))
	{
		perror("setuid");
		return 1;
	}
	char **args;
	if (argc < 2)
	{
		char *def[] = { "/system/bin/sh", NULL };
		args = def;
	}
	else
	{
		args = &argv[1];
	}
	execvp(args[0], args);
	perror("execvp");
	return 127;
}
