#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pty.h>
#include <curl/curl.h>
#include "leaf_parser.h"

#define BASE_URL "https://leaf.treelinux.org"
#define API_ENDPOINT "/api/package/"
#define DOWNLOAD_ENDPOINT "/userfiles/"

// Cached paths (computed once)
static const char *g_home = NULL;
static char g_cache_dir[512];
static char g_packages_dir[512];
static char g_log_path[512];
static int g_paths_init = 0;

static int init_paths(void) {
	if (g_paths_init) return 0;
	g_home = getenv("HOME");
	if (!g_home) {
		fprintf(stderr, "Cannot determine home directory\n");
		return -1;
	}
	snprintf(g_cache_dir, sizeof(g_cache_dir), "%s/.leaf/cache", g_home);
	snprintf(g_packages_dir, sizeof(g_packages_dir), "%s/leaf/packages", g_home);
	snprintf(g_log_path, sizeof(g_log_path), "%s/.leaf/log.txt", g_home);
	
	// Ensure directories exist
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s/.leaf", g_home);
	mkdir(tmp, 0755);
	mkdir(g_cache_dir, 0755);
	snprintf(tmp, sizeof(tmp), "%s/leaf", g_home);
	mkdir(tmp, 0755);
	mkdir(g_packages_dir, 0755);
	
	g_paths_init = 1;
	return 0;
}

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

// Buffer for HTTP responses (pre-allocated with exponential growth)
typedef struct {
	char *data;
	size_t size;
	size_t capacity;
} Buffer;

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
	size_t realsize = size * nmemb;
	Buffer *buf = (Buffer *)userp;
	size_t needed = buf->size + realsize + 1;
	
	if (needed > buf->capacity) {
		size_t newcap = buf->capacity ? buf->capacity : 4096;
		while (newcap < needed) newcap *= 2;
		char *ptr = realloc(buf->data, newcap);
		if (!ptr) return 0;
		buf->data = ptr;
		buf->capacity = newcap;
	}
	
	memcpy(buf->data + buf->size, contents, realsize);
	buf->size += realsize;
	buf->data[buf->size] = 0;
	return realsize;
}

static size_t write_file_callback(void *contents, size_t size, size_t nmemb, void *userp) {
	return fwrite(contents, size, nmemb, (FILE *)userp);
}

// Simple JSON field extraction (no full parser needed)
static char *json_get_string(const char *json, const char *key) {
	char search[256];
	snprintf(search, sizeof(search), "\"%s\":", key);
	
	const char *pos = strstr(json, search);
	if (!pos) return NULL;
	
	pos += strlen(search);
	while (*pos == ' ' || *pos == '\t') pos++;
	
	if (*pos != '"') return NULL;
	pos++;
	
	const char *end = strchr(pos, '"');
	if (!end) return NULL;
	
	size_t len = end - pos;
	char *result = malloc(len + 1);
	if (!result) return NULL;
	
	strncpy(result, pos, len);
	result[len] = 0;
	return result;
}

static int json_get_bool(const char *json, const char *key) {
	char search[256];
	snprintf(search, sizeof(search), "\"%s\":", key);
	
	const char *pos = strstr(json, search);
	if (!pos) return -1;
	
	pos += strlen(search);
	while (*pos == ' ' || *pos == '\t') pos++;
	
	if (strncmp(pos, "true", 4) == 0) return 1;
	if (strncmp(pos, "false", 5) == 0) return 0;
	return -1;
}

// Check if a command exists in PATH by scanning directories directly (no fork/exec)
static int command_exists(const char *cmd_name) {
	const char *path_env = getenv("PATH");
	if (!path_env) return 0;
	
	char pathbuf[4096];
	size_t plen = strlen(path_env);
	if (plen >= sizeof(pathbuf)) plen = sizeof(pathbuf) - 1;
	memcpy(pathbuf, path_env, plen);
	pathbuf[plen] = '\0';
	
	char *saveptr;
	char *dir = strtok_r(pathbuf, ":", &saveptr);
	while (dir) {
		char fullpath[512];
		snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, cmd_name);
		if (access(fullpath, X_OK) == 0) return 1;
		dir = strtok_r(NULL, ":", &saveptr);
	}
	return 0;
}

// Check if a package/dependency is installed
// Checks: 1) system PATH (bin directories), 2) ~/leaf/packages
static int is_package_installed(const char *pkg_name) {
	if (command_exists(pkg_name)) return 1;
	
	if (init_paths() != 0) return 0;
	
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", g_packages_dir, pkg_name);
	struct stat st;
	return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

// Forward declarations
static int get_terminal_width(void);
static void draw_progress(const char *msg, int frame);

// Draw a real progress bar with known percentage (buffered single write)
static void draw_real_progress(const char *msg, const char *stage, int percent) {
	static int bounce_pos = 0;
	int term_width = get_terminal_width();
	
	int msg_len = (int)strlen(msg);
	int stage_len = stage ? (int)strlen(stage) + 3 : 0;
	// percent < 0 means indeterminate (no dry-run data)
	int tail_len = (percent >= 0) ? 5 : 0; // " NN%" or nothing
	int bar_width = term_width - 1 - msg_len - stage_len - 1 - 2 - tail_len;
	if (bar_width < 10) bar_width = 10;
	
	char buf[2048];
	int pos = 0;
	pos += snprintf(buf + pos, sizeof(buf) - pos, "\r\033[K%s", msg);
	if (stage) pos += snprintf(buf + pos, sizeof(buf) - pos, " - %s", stage);
	pos += snprintf(buf + pos, sizeof(buf) - pos, " [\033[1;32m");
	
	if (percent >= 0) {
		// Determinate: real percentage bar
		if (percent > 100) percent = 100;
		int bar_fill = (percent * bar_width) / 100;
		for (int i = 0; i < bar_fill && pos < (int)sizeof(buf) - 20; i++) buf[pos++] = '=';
		if (bar_fill < bar_width && percent < 100 && pos < (int)sizeof(buf) - 20) buf[pos++] = '>';
		pos += snprintf(buf + pos, sizeof(buf) - pos, "\033[0m");
		int spaces = bar_width - bar_fill - (percent < 100 ? 1 : 0);
		for (int i = 0; i < spaces && pos < (int)sizeof(buf) - 20; i++) buf[pos++] = ' ';
		pos += snprintf(buf + pos, sizeof(buf) - pos, "] %3d%%", percent);
	} else {
		// Indeterminate: bouncing pulse (real line count shown in stage)
		int pulse_w = bar_width / 5;
		if (pulse_w < 3) pulse_w = 3;
		bounce_pos = (bounce_pos + 1) % ((bar_width - pulse_w) * 2);
		int bp = bounce_pos;
		if (bp >= bar_width - pulse_w) bp = (bar_width - pulse_w) * 2 - bp;
		for (int i = 0; i < bar_width && pos < (int)sizeof(buf) - 20; i++) {
			buf[pos++] = (i >= bp && i < bp + pulse_w) ? '=' : ' ';
		}
		pos += snprintf(buf + pos, sizeof(buf) - pos, "\033[0m]");
	}
	write(STDOUT_FILENO, buf, pos);
}

// Parse git progress output for percentage
// Git outputs lines like: "Receiving objects:  45% (123/274)"
static int parse_git_progress(const char *line, char *stage_out, size_t stage_size) {
	// Look for percentage pattern: "Stage: NN%"
	const char *pct = strstr(line, "%");
	if (!pct) return -1;
	
	// Find the number before %
	const char *num_start = pct - 1;
	while (num_start > line && isdigit((unsigned char)*num_start)) num_start--;
	if (!isdigit((unsigned char)*num_start)) num_start++;
	
	if (num_start >= pct) return -1;
	
	int percent = atoi(num_start);
	
	// Extract stage name (before the colon)
	const char *colon = strchr(line, ':');
	if (colon && stage_out && stage_size > 0) {
		size_t len = colon - line;
		if (len >= stage_size) len = stage_size - 1;
		// Skip "remote: " prefix if present
		const char *start = line;
		if (strncmp(line, "remote: ", 8) == 0) {
			start = line + 8;
			len = colon - start;
			if (len >= stage_size) len = stage_size - 1;
		}
		strncpy(stage_out, start, len);
		stage_out[len] = '\0';
	}
	
	return percent;
}

// Clone a git repository with real progress
static int git_clone(const char *github_url, const char *dest_dir) {
	if (init_paths() != 0) return -1;
	
	// Create pipe for reading git's progress
	int pipefd[2];
	if (pipe(pipefd) == -1) {
		return -1;
	}
	
	pid_t pid = fork();
	if (pid == -1) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	
	if (pid == 0) {
		// Child process
		close(pipefd[0]); // Close read end
		
		// Also log to file
		FILE *logf = fopen(g_log_path, "a");
		
		// Redirect stderr to pipe (git progress goes to stderr)
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		
		// Redirect stdout to log or /dev/null
		if (logf) {
			dup2(fileno(logf), STDOUT_FILENO);
			fclose(logf);
		} else {
			freopen("/dev/null", "w", stdout);
		}
		
		execlp("git", "git", "clone", "--depth", "1", "--progress", github_url, dest_dir, (char *)NULL);
		_exit(127);
	}
	
	// Parent process
	close(pipefd[1]); // Close write end
	
	// Open log file for appending git output
	FILE *logf = fopen(g_log_path, "a");
	
	// Set pipe to non-blocking
	int flags = fcntl(pipefd[0], F_GETFL, 0);
	fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
	
	char buffer[512];
	char stage[64] = "Starting";
	int percent = 0;
	int status;
	
	draw_real_progress("Cloning", stage, percent);
	
	while (1) {
		pid_t result = waitpid(pid, &status, WNOHANG);
		
		// Read any available output from git
		ssize_t n;
		while ((n = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
			buffer[n] = '\0';
			
			// Log to file
			if (logf) {
				fputs(buffer, logf);
				fflush(logf);
			}
			
			// Parse for progress - git uses \r for progress updates
			char *line = buffer;
			char *cr;
			while ((cr = strchr(line, '\r')) != NULL || (cr = strchr(line, '\n')) != NULL) {
				*cr = '\0';
				char new_stage[64];
				int new_pct = parse_git_progress(line, new_stage, sizeof(new_stage));
				if (new_pct >= 0) {
					percent = new_pct;
					if (new_stage[0]) {
						strncpy(stage, new_stage, sizeof(stage) - 1);
						stage[sizeof(stage) - 1] = '\0';
					}
				}
				line = cr + 1;
			}
			// Check remainder
			if (*line) {
				char new_stage[64];
				int new_pct = parse_git_progress(line, new_stage, sizeof(new_stage));
				if (new_pct >= 0) {
					percent = new_pct;
					if (new_stage[0]) {
						strncpy(stage, new_stage, sizeof(stage) - 1);
						stage[sizeof(stage) - 1] = '\0';
					}
				}
			}
		}
		
		if (result == pid) break;
		if (result == -1 && errno != ECHILD) {
			write(STDOUT_FILENO, "\r\033[K", 4);
			close(pipefd[0]);
			if (logf) fclose(logf);
			return -1;
		}
		
		draw_real_progress("Cloning", stage, percent);
		struct timespec ts = {0, 100000000}; // 100ms
		nanosleep(&ts, NULL);
	}
	
	close(pipefd[0]);
	if (logf) fclose(logf);
	
	write(STDOUT_FILENO, "\r\033[K", 4);
	
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		printf("\033[1;32m✓\033[0m Repository cloned\n");
		return 0;
	}
	return -1;
}

// Forward declaration for recursive dependency installation
static int install_package(const char *pkg_name);

// Install dependencies for a package
static int install_dependencies(leaf_manifest *manifest) {
	if (!manifest || manifest->dependency_count == 0) {
		return 0;
	}
	
	printf("Checking %zu dependencies...\n", manifest->dependency_count);
	
	for (size_t i = 0; i < manifest->dependency_count; i++) {
		const char *dep = manifest->dependencies[i];
		
		if (is_package_installed(dep)) {
			printf("  [✓] %s (already installed)\n", dep);
		} else {
			printf("  [↓] %s (installing...)\n", dep);
			if (install_package(dep) != 0) {
				fprintf(stderr, "Failed to install dependency: %s\n", dep);
				return -1;
			}
		}
	}
	
	printf("All dependencies satisfied.\n");
	return 0;
}

// Get terminal width (cached, refreshed every 20 calls)
static int g_term_width = 0;
static int g_term_width_calls = 0;

static int get_terminal_width(void) {
	if (g_term_width && g_term_width_calls++ < 20) return g_term_width;
	g_term_width_calls = 0;
	struct winsize w;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
		g_term_width = w.ws_col;
		return w.ws_col;
	}
	g_term_width = 80;
	return 80;
}

// Draw an indeterminate progress bar (buffered single write)
static void draw_progress(const char *msg, int frame) {
	const char spinner[] = "|/-\\";
	int term_width = get_terminal_width();
	
	int msg_len = (int)strlen(msg);
	int bar_width = term_width - 1 - 1 - msg_len - 1 - 2 - 3;
	if (bar_width < 10) bar_width = 10;
	
	int block_width = 5;
	int travel = bar_width - block_width;
	if (travel < 1) travel = 1;
	int pos = frame % (travel * 2);
	if (pos >= travel) pos = travel * 2 - pos;
	
	int dots = (frame / 3) % 4;
	
	// Build entire line in buffer, single write
	char buf[2048];
	int p = 0;
	p += snprintf(buf + p, sizeof(buf) - p, "\r\033[K%c %s [", spinner[frame % 4], msg);
	for (int i = 0; i < bar_width && p < (int)sizeof(buf) - 30; i++) {
		if (i >= pos && i < pos + block_width) {
			buf[p++] = '\033'; buf[p++] = '['; buf[p++] = '1'; buf[p++] = ';';
			buf[p++] = '3'; buf[p++] = '2'; buf[p++] = 'm'; buf[p++] = '=';
			buf[p++] = '\033'; buf[p++] = '['; buf[p++] = '0'; buf[p++] = 'm';
		} else {
			buf[p++] = ' ';
		}
	}
	buf[p++] = ']';
	for (int i = 0; i < dots; i++) buf[p++] = '.';
	for (int i = dots; i < 3; i++) buf[p++] = ' ';
	write(STDOUT_FILENO, buf, p);
}

// Parse cmake-style progress marker "[ NN%]" from a line, return percentage or -1
// Handles ANSI escape codes from pty output (e.g. \033[32m[ 42%]\033[0m)
static int parse_cmake_progress(const char *line) {
	const char *p = line;
	// Skip whitespace and ANSI escape sequences
	while (*p) {
		if (*p == ' ' || *p == '\t') { p++; continue; }
		if (*p == '\033') {
			p++;
			if (*p == '[') { p++; while (*p && *p != 'm') p++; if (*p) p++; }
			continue;
		}
		break;
	}
	if (*p != '[') return -1;
	p++;
	// Skip ANSI inside brackets too
	while (*p == '\033') { p++; if (*p == '[') { p++; while (*p && *p != 'm') p++; if (*p) p++; } }
	while (*p == ' ') p++;
	int val = 0;
	int has_digit = 0;
	while (*p >= '0' && *p <= '9') {
		val = val * 10 + (*p - '0');
		has_digit = 1;
		p++;
	}
	if (!has_digit || *p != '%') return -1;
	p++;
	// Skip ANSI before closing bracket
	while (*p == '\033') { p++; if (*p == '[') { p++; while (*p && *p != 'm') p++; if (*p) p++; } }
	if (*p != ']') return -1;
	return (val > 100) ? 100 : val;
}

// Run compile command in package directory (real progress via pty)
static int compile_package(const char *pkg_dir, const char *compile_cmd) {
	if (!compile_cmd || !*compile_cmd) {
		return 0;
	}
	
	if (init_paths() != 0) return -1;
	
	// Use a pty so build tools (cmake/make/ninja) think they have a terminal
	// and emit real-time progress markers like [ NN%] instead of buffering
	int master_fd;
	pid_t pid = forkpty(&master_fd, NULL, NULL, NULL);
	if (pid == -1) return -1;
	
	if (pid == 0) {
		// Child has pty as stdin/stdout/stderr automatically
		if (chdir(pkg_dir) != 0) _exit(127);
		execl("/bin/sh", "sh", "-c", compile_cmd, (char *)NULL);
		_exit(127);
	}
	
	// Parent reads from master_fd (the pty master side)
	int logfd = open(g_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	
	int fl = fcntl(master_fd, F_GETFL, 0);
	fcntl(master_fd, F_SETFL, fl | O_NONBLOCK);
	
	char readbuf[4096];
	char linebuf[2048];
	int linepos = 0;
	int cmake_pct = 0;         // current cmake phase percentage
	int cmake_prev_pct = -1;   // previous to detect phase resets
	int cmake_phase = 0;       // how many completed phases (0→100→reset)
	int uses_cmake = 0;        // have we seen [ NN%] markers?
	int stale_ticks = 0;       // ticks since last cmake marker update
	int total_lines = 0;       // total non-empty output lines (for indeterminate)
	int status;
	
	draw_real_progress("Compiling", NULL, -1);
	
	for (;;) {
		pid_t result = waitpid(pid, &status, WNOHANG);
		
		ssize_t n;
		int got_new_marker = 0;
		while ((n = read(master_fd, readbuf, sizeof(readbuf) - 1)) > 0) {
			if (logfd >= 0) write(logfd, readbuf, n);
			readbuf[n] = '\0';
			
			for (int i = 0; i < n; i++) {
				if (readbuf[i] == '\n' || readbuf[i] == '\r') {
					linebuf[linepos] = '\0';
					if (linepos > 0) {
						total_lines++;
						int cp = parse_cmake_progress(linebuf);
						if (cp >= 0) {
							uses_cmake = 1;
							got_new_marker = 1;
							// Detect phase reset: percentage drops significantly
							if (cmake_prev_pct >= 90 && cp < 20) {
								cmake_phase++;
							}
							cmake_pct = cp;
							cmake_prev_pct = cp;
						}
					}
					linepos = 0;
				} else if (linepos < (int)sizeof(linebuf) - 1) {
					linebuf[linepos++] = readbuf[i];
				}
			}
		}
		
		if (got_new_marker) {
			stale_ticks = 0;
		} else {
			stale_ticks++;
		}
		
		if (result == pid || (result == -1 && errno == ECHILD)) {
			// Process exited — drain remaining pty output
			fcntl(master_fd, F_SETFL, fl);
			// Short reads with timeout — pty returns EIO when slave closes
			for (int drain = 0; drain < 50; drain++) {
				n = read(master_fd, readbuf, sizeof(readbuf) - 1);
				if (n <= 0) break;
				if (logfd >= 0) write(logfd, readbuf, n);
			}
			if (result == -1) {
				// ECHILD: already reaped
				waitpid(pid, &status, 0);
			}
			break;
		}
		
		if (result == -1 && errno != ECHILD) {
			write(STDOUT_FILENO, "\r\033[K", 4);
			close(master_fd);
			if (logfd >= 0) close(logfd);
			return -1;
		}
		
		// Display progress
		if (uses_cmake) {
			if (stale_ticks < 40) {
				// Fresh cmake markers — show real percentage
				char phase_label[48];
				if (cmake_phase > 0) {
					snprintf(phase_label, sizeof(phase_label), "phase %d", cmake_phase + 1);
					draw_real_progress("Compiling", phase_label, cmake_pct);
				} else {
					draw_real_progress("Compiling", NULL, cmake_pct);
				}
			} else {
				// Stale: cmake hit 100% on a sub-build or configure is running
				// Switch to indeterminate until new markers arrive
				draw_real_progress("Compiling", "configuring", -1);
			}
		} else if (total_lines > 0) {
			char stage[32];
			snprintf(stage, sizeof(stage), "%d steps", total_lines);
			draw_real_progress("Compiling", stage, -1);
		} else {
			draw_real_progress("Compiling", NULL, -1);
		}
		
		struct timespec ts = {0, 50000000}; // 50ms
		nanosleep(&ts, NULL);
	}
	
	close(master_fd);
	if (logfd >= 0) close(logfd);
	
	write(STDOUT_FILENO, "\r\033[K", 4);
	
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		draw_real_progress("Compiling", NULL, 100);
		struct timespec ts = {0, 200000000};
		nanosleep(&ts, NULL);
		write(STDOUT_FILENO, "\r\033[K", 4);
		printf("\033[1;32m✓\033[0m Compilation successful!\n");
		return 0;
	} else {
		int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		fprintf(stderr, "\033[1;31m✗\033[0m Compilation failed (exit code %d)\n", exit_code);
		fprintf(stderr, "  See log: %s\n", g_log_path);
		return -1;
	}
}

static void print_version(void) {
	printf("leaf 1.0.0\n");
}

static void print_usage(const char *prog) {
	fprintf(stderr,
			"Usage: %s <command> [args]\n\n"
			"Commands:\n"
			"  grow <pkg>    Install a package (clone, install deps, compile)\n"
			"  uproot <pkg>  Remove a package from ~/leaf/packages\n"
			"  list          List installed packages (placeholder)\n"
			"  reset         Reset leaf state (placeholder)\n\n"
			"Global options:\n"
			"  -v, --version     Show version\n"
			"  -h, --help        Show this help\n\n",
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

	if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
		print_usage(argv[0]);
		exit(0);
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

// Fetch package info from API
static int fetch_package_info(const char *pkg, char **username, char **filename) {
	CURL *curl;
	CURLcode res;
	Buffer buf = {0};
	int ret = -1;
	
	char url[512];
	snprintf(url, sizeof(url), "%s%s%s", BASE_URL, API_ENDPOINT, pkg);
	
	curl = curl_easy_init();
	if (!curl) {
		fprintf(stderr, "Failed to initialize curl\n");
		return -1;
	}
	
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	
	res = curl_easy_perform(curl);
	
	if (res != CURLE_OK) {
		fprintf(stderr, "Failed to connect: %s\n", curl_easy_strerror(res));
		goto cleanup;
	}
	
	long http_code;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	
	if (http_code == 404) {
		fprintf(stderr, "Package '%s' not found\n", pkg);
		goto cleanup;
	}
	
	if (http_code != 200) {
		fprintf(stderr, "Server error: HTTP %ld\n", http_code);
		goto cleanup;
	}
	
	// Parse JSON response
	int found = json_get_bool(buf.data, "found");
	if (found != 1) {
		char *error = json_get_string(buf.data, "error");
		fprintf(stderr, "%s\n", error ? error : "Package not found");
		free(error);
		goto cleanup;
	}
	
	*username = json_get_string(buf.data, "username");
	*filename = json_get_string(buf.data, "filename");
	
	if (!*username || !*filename) {
		fprintf(stderr, "Invalid response from server\n");
		free(*username);
		free(*filename);
		*username = NULL;
		*filename = NULL;
		goto cleanup;
	}
	
	ret = 0;
	
cleanup:
	curl_easy_cleanup(curl);
	free(buf.data);
	return ret;
}

// Download file to cache
static int download_package(const char *username, const char *filename, const char *cache_dir) {
	CURL *curl;
	CURLcode res;
	int ret = -1;
	
	char url[512];
	snprintf(url, sizeof(url), "%s%s%s/%s", BASE_URL, DOWNLOAD_ENDPOINT, username, filename);
	
	char filepath[512];
	snprintf(filepath, sizeof(filepath), "%s/%s", cache_dir, filename);
	
	// Check if already cached
	struct stat st;
	if (stat(filepath, &st) == 0) {
		printf("Package already cached: %s\n", filepath);
		return 0;
	}
	
	printf("Downloading %s...\n", filename);
	
	FILE *fp = fopen(filepath, "wb");
	if (!fp) {
		fprintf(stderr, "Cannot create file: %s\n", filepath);
		return -1;
	}
	
	curl = curl_easy_init();
	if (!curl) {
		fprintf(stderr, "Failed to initialize curl\n");
		fclose(fp);
		return -1;
	}
	
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
	
	res = curl_easy_perform(curl);
	fclose(fp);
	
	if (res != CURLE_OK) {
		fprintf(stderr, "Download failed: %s\n", curl_easy_strerror(res));
		remove(filepath);
		goto cleanup;
	}
	
	long http_code;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	
	if (http_code != 200) {
		fprintf(stderr, "Download failed: HTTP %ld\n", http_code);
		remove(filepath);
		goto cleanup;
	}
	
	printf("Downloaded to: %s\n", filepath);
	ret = 0;
	
cleanup:
	curl_easy_cleanup(curl);
	return ret;
}

// Main package installation function (can be called recursively for dependencies)
static int install_package(const char *pkg_name) {
	char *username = NULL;
	char *filename = NULL;
	leaf_manifest *manifest = NULL;
	char filepath[512];
	char pkg_dest[512];
	int ret = 1;
	
	if (init_paths() != 0) return 1;
	
	// Check if already installed
	if (is_package_installed(pkg_name)) {
		printf("Package '%s' is already installed.\n", pkg_name);
		return 0;
	}
	
	// Find package in repository
	printf("\n=== Installing: %s ===\n", pkg_name);
	printf("Searching for package '%s'...\n", pkg_name);
	if (fetch_package_info(pkg_name, &username, &filename) != 0) {
		goto cleanup;
	}
	
	printf("Found: %s by %s\n", filename, username);
	
	// Download package manifest
	if (download_package(username, filename, g_cache_dir) != 0) {
		goto cleanup;
	}
	
	// Parse manifest
	snprintf(filepath, sizeof(filepath), "%s/%s", g_cache_dir, filename);
	manifest = parse_leaf_file(filepath);
	if (!manifest) {
		fprintf(stderr, "Error: Could not parse manifest\n");
		goto cleanup;
	}
	
	printf("\n");
	print_leaf_manifest(manifest);
	
	// Check if GitHub URL is provided
	if (!manifest->github || !*manifest->github) {
		fprintf(stderr, "Error: No GitHub URL in manifest\n");
		goto cleanup;
	}
	
	// Install dependencies first
	if (install_dependencies(manifest) != 0) {
		goto cleanup;
	}
	
	// Clone repository to ~/leaf/packages/<name>
	const char *name = manifest->name ? manifest->name : pkg_name;
	snprintf(pkg_dest, sizeof(pkg_dest), "%s/%s", g_packages_dir, name);
	
	struct stat st;
	if (stat(pkg_dest, &st) == 0) {
		printf("\nPackage directory already exists, skipping clone.\n");
	} else {
		printf("\nCloning repository...\n");
		if (git_clone(manifest->github, pkg_dest) != 0) {
			fprintf(stderr, "Failed to clone repository\n");
			goto cleanup;
		}
	}
	
	// Run compile command
	if (compile_package(pkg_dest, manifest->compile_cmd) != 0) {
		goto cleanup;
	}
	
	printf("\n=== Successfully installed: %s ===\n", name);
	ret = 0;
	
cleanup:
	free(username);
	free(filename);
	if (manifest) free_leaf_manifest(manifest);
	return ret;
}

static int cmd_grow(const Options *o) {
	curl_global_init(CURL_GLOBAL_DEFAULT);
	int ret = install_package(o->pkg);
	curl_global_cleanup();
	return ret;
}

static int cmd_uproot(const Options *o) {
	const char *pkg_name = o->pkg;
	if (init_paths() != 0) return 1;
	
	char pkg_path[512];
	snprintf(pkg_path, sizeof(pkg_path), "%s/%s", g_packages_dir, pkg_name);
	
	struct stat st;
	if (stat(pkg_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "Package '%s' is not installed.\n", pkg_name);
		return 1;
	}
	
	printf("\n=== Removing: %s ===\n", pkg_name);
	printf("Location: %s\n\n", pkg_path);
	
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s' 2>/dev/null", pkg_path);
	
	pid_t pid = fork();
	if (pid == -1) {
		fprintf(stderr, "Failed to fork\n");
		return 1;
	}
	
	if (pid == 0) {
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	
	int status;
	int frame = 0;
	while (1) {
		pid_t result = waitpid(pid, &status, WNOHANG);
		if (result == pid) break;
		if (result == -1) {
			write(STDOUT_FILENO, "\r\033[K", 4);
			fprintf(stderr, "waitpid error\n");
			return 1;
		}
		draw_progress("Removing", frame++);
		struct timespec ts = {0, 100000000};
		nanosleep(&ts, NULL);
	}
	
	write(STDOUT_FILENO, "\r\033[K", 4);
	
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		printf("\033[1;32m✓\033[0m Successfully removed '%s'\n", pkg_name);
		return 0;
	} else {
		int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		fprintf(stderr, "\033[1;31m✗\033[0m Failed to remove package (exit code %d)\n", exit_code);
		return 1;
	}
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
