#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

typedef enum {
	CMD_NONE = 0,
	CMD_GROW,
	CMD_UPROOT,
	CMD_LIST,
	CMD_RESET
} Command;

typedef struct {
	Command cmd;
	const char *pkg;      // package name for grow/uproot
	int version;
} Options;

static void print_version(void) {
	printf("[version] placeholder\n");
}

static void print_usage(const char *prog) {
	fprintf(stderr,
			"Usage: %s <command> [args]\n\n"
			"Commands:\n"
			"  grow      Grow <pkg> (placeholder)\n"
			"  uproot    Uproot <pkg> (placeholder)\n"
			"  list      List (placeholder)\n"
			"  reset     Reset (placeholder)\n\n"
			"Global options:\n"
			"  -v, --version     Show version\n\n",
			prog);
}

static int parse_args(int argc, char **argv, Options *opts) {
	if (argc < 2) {
		print_usage(argv[0]);
		return -1;
	}
	memset(opts, 0, sizeof(*opts));

	if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
		opts->version = 1;
		return 0;
	}

	// Command dispatch
	const char *cmd = argv[1];
	if (strcmp(cmd, "grow") == 0) {
		opts->cmd = CMD_GROW;
		if (argc < 3) {
			fprintf(stderr, "grow requires <pkg>\n");
			return -1;
		}
		opts->pkg = argv[2];
		// no extra args expected beyond <pkg>
		if (argc > 3) {
			fprintf(stderr, "grow takes exactly one argument: <pkg>\n");
			return -1;
		}
	} else if (strcmp(cmd, "uproot") == 0) {
		opts->cmd = CMD_UPROOT;
		if (argc < 3) {
			fprintf(stderr, "uproot requires <pkg>\n");
			return -1;
		}
		opts->pkg = argv[2];
		if (argc > 3) {
			fprintf(stderr, "uproot takes exactly one argument: <pkg>\n");
			return -1;
		}
	} else if (strcmp(cmd, "list") == 0) {
		opts->cmd = CMD_LIST;
		if (argc > 2) {
			fprintf(stderr, "list takes no arguments\n");
			return -1;
		}
	} else if (strcmp(cmd, "reset") == 0) {
		opts->cmd = CMD_RESET;
		if (argc > 2) {
			fprintf(stderr, "reset takes no arguments\n");
			return -1;
		}
	} else {
		fprintf(stderr, "Unknown command: %s\n", cmd);
		print_usage(argv[0]);
		return -1;
	}

	return 0;
}

// Stubs for future implementation

static int cmd_grow(const Options *o) {
	printf("[grow] pkg=%s (placeholder)\n", o->pkg);
	return 0;
}

static int cmd_uproot(const Options *o) {
	printf("[uproot] pkg=%s (placeholder)\n", o->pkg);
	return 0;
}

static int cmd_list(const Options *o) {
	(void)o;
	printf("[list] placeholder\n");
	return 0;
}

static int cmd_reset(const Options *o) {
	(void)o;
	printf("[reset] placeholder\n");
	return 0;
}

int main(int argc, char **argv) {
	Options opts;
	if (argc <= 1) {
		print_usage(argv[0]);
		return 1;
	}
	if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
		print_version();
		return 0;
	}

	if (parse_args(argc, argv, &opts) != 0) {
		// parse_args already printed why
		return 2;
	}

	if (opts.version) { print_version(); return 0; }

	switch (opts.cmd) {
		case CMD_GROW: return cmd_grow(&opts);
		case CMD_UPROOT: return cmd_uproot(&opts);
		case CMD_LIST: return cmd_list(&opts);
		case CMD_RESET: return cmd_reset(&opts);
		default:
			print_usage(argv[0]);
			return 2;
	}
}
