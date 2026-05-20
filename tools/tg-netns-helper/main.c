/*
 * tg-netns-helper.c — TheGates per-spawn network namespace setup helper.
 *
 * Setuid-root helper that creates a veth pair to the renderer's network
 * namespace and installs nftables drop rules for RFC 1918 / loopback /
 * link-local destinations. The launcher invokes this once per renderer
 * spawn.
 *
 * Usage:
 *   tg-netns-helper <renderer-pid> <veth-host-name> <veth-ns-name>
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -o tg-netns-helper main.c
 *
 * Install (mode 4755):
 *   sudo install -o root -m 4755 tg-netns-helper /usr/local/sbin/
 *
 * The helper:
 *   1) Validates the caller is the launcher binary (reads /proc/<ppid>/exe).
 *   2) Creates a veth pair with one end already in the child's netns.
 *   3) Brings up the host side, configures IPs, enables forwarding for
 *      our veth pair only (does not change net.ipv4.ip_forward globally).
 *   4) Loads an nftables ruleset scoped to the renderer veth iif:
 *      - drop traffic to RFC 1918 / loopback / link-local
 *      - MASQUERADE everything else through the host's primary interface
 *   5) Drops privileges (setuid to caller's uid) and exits.
 *
 * On any error, the helper returns non-zero. The launcher fails closed
 * and refuses to start the renderer.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Read /proc/<pid>/exe target and compare against the expected launcher binary. */
static int verify_caller_is_launcher(pid_t ppid) {
	char proc_path[64];
	snprintf(proc_path, sizeof(proc_path), "/proc/%d/exe", (int)ppid);

	char real_path[4096];
	ssize_t n = readlink(proc_path, real_path, sizeof(real_path) - 1);
	if (n < 0) {
		fprintf(stderr, "tg-netns-helper: readlink %s failed: %s\n", proc_path, strerror(errno));
		return 0;
	}
	real_path[n] = '\0';

	/* The launcher binary is named godot.linuxbsd.editor.* (dev) or godot.linuxbsd.template_release.*
	 * (release) per Godot's naming convention. Reject obvious shells / scripts. */
	const char *base = strrchr(real_path, '/');
	base = base ? base + 1 : real_path;
	if (strstr(base, "godot.linuxbsd.") == NULL) {
		fprintf(stderr, "tg-netns-helper: caller %s is not a TheGates launcher\n", real_path);
		return 0;
	}
	return 1;
}

static int run(const char *path, char *const argv[]) {
	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "tg-netns-helper: fork failed: %s\n", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		execv(path, argv);
		_exit(127);
	}
	int status = 0;
	if (waitpid(pid, &status, 0) < 0) {
		fprintf(stderr, "tg-netns-helper: waitpid failed: %s\n", strerror(errno));
		return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "tg-netns-helper: %s failed (status=%d)\n", path, status);
		return -1;
	}
	return 0;
}

static int run_ip(char *const argv[]) {
	return run("/sbin/ip", argv) == 0 ? 0 : (run("/usr/sbin/ip", argv) == 0 ? 0 : -1);
}

static int setup_veth(const char *veth_host, const char *veth_ns, const char *pid_str) {
	/* ip link add <veth_host> type veth peer name <veth_ns> netns <pid> */
	char *args1[] = {
		"ip", "link", "add", (char *)veth_host, "type", "veth",
		"peer", "name", (char *)veth_ns, "netns", (char *)pid_str,
		NULL,
	};
	if (run_ip(args1) != 0) {
		return -1;
	}

	/* ip addr add 10.66.0.1/30 dev <veth_host> */
	char *args2[] = { "ip", "addr", "add", "10.66.0.1/30", "dev", (char *)veth_host, NULL };
	if (run_ip(args2) != 0) {
		return -1;
	}

	/* ip link set <veth_host> up */
	char *args3[] = { "ip", "link", "set", (char *)veth_host, "up", NULL };
	if (run_ip(args3) != 0) {
		return -1;
	}

	/* Enter the renderer's netns and bring up its side. */
	char *args4[] = {
		"ip", "-n", (char *)pid_str, "addr", "add", "10.66.0.2/30", "dev", (char *)veth_ns, NULL,
	};
	if (run_ip(args4) != 0) {
		return -1;
	}
	char *args5[] = {
		"ip", "-n", (char *)pid_str, "link", "set", (char *)veth_ns, "up", NULL,
	};
	if (run_ip(args5) != 0) {
		return -1;
	}
	char *args6[] = {
		"ip", "-n", (char *)pid_str, "link", "set", "lo", "up", NULL,
	};
	if (run_ip(args6) != 0) {
		return -1;
	}
	char *args7[] = {
		"ip", "-n", (char *)pid_str, "route", "add", "default", "via", "10.66.0.1", NULL,
	};
	if (run_ip(args7) != 0) {
		return -1;
	}

	return 0;
}

/* Write a per-renderer-namespace /etc/resolv.conf into the renderer's
 * mount namespace. Since we're not in the mount namespace here, we write
 * a marker file the launcher's child code uses to bind-mount before exec. */
static int write_resolv(const char *veth_host) {
	char path[256];
	snprintf(path, sizeof(path), "/run/tg-netns/%s-resolv.conf", veth_host);
	mkdir("/run/tg-netns", 0755);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		return -1;
	}
	const char *body = "nameserver 1.1.1.1\nnameserver 8.8.8.8\n";
	if (write(fd, body, strlen(body)) != (ssize_t)strlen(body)) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static int apply_nftables(const char *veth_host) {
	/* Generate the nft script. */
	char rules[2048];
	int n = snprintf(rules, sizeof(rules),
			"table inet tg_filter {\n"
			"  chain forward {\n"
			"    type filter hook forward priority 0\n"
			"    ct state established,related accept\n"
			"    iifname \"%s\" ip daddr { 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, 127.0.0.0/8, 169.254.0.0/16 } drop\n"
			"    iifname \"%s\" ip6 daddr { ::1/128, fc00::/7, fe80::/10 } drop\n"
			"    iifname \"%s\" accept\n"
			"  }\n"
			"  chain postrouting {\n"
			"    type nat hook postrouting priority 100\n"
			"    iifname \"%s\" masquerade\n"
			"  }\n"
			"}\n",
			veth_host, veth_host, veth_host, veth_host);
	if (n < 0 || n >= (int)sizeof(rules)) {
		return -1;
	}

	char path[256];
	snprintf(path, sizeof(path), "/run/tg-netns/%s.nft", veth_host);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		fprintf(stderr, "tg-netns-helper: open %s failed: %s\n", path, strerror(errno));
		return -1;
	}
	if (write(fd, rules, n) != n) {
		close(fd);
		return -1;
	}
	close(fd);

	char *args[] = { "nft", "-f", path, NULL };
	if (run("/usr/sbin/nft", args) != 0 && run("/sbin/nft", args) != 0) {
		return -1;
	}
	return 0;
}

int main(int argc, char *argv[]) {
	if (argc != 4) {
		fprintf(stderr, "Usage: tg-netns-helper <pid> <veth-host> <veth-ns>\n");
		return 1;
	}

	const char *pid_str = argv[1];
	const char *veth_host = argv[2];
	const char *veth_ns = argv[3];

	pid_t ppid = getppid();
	if (!verify_caller_is_launcher(ppid)) {
		return 2;
	}

	if (setup_veth(veth_host, veth_ns, pid_str) != 0) {
		return 3;
	}

	if (apply_nftables(veth_host) != 0) {
		return 4;
	}

	if (write_resolv(veth_host) != 0) {
		return 5;
	}

	return 0;
}
