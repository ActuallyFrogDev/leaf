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
#include <curl/curl.h>
#include "leaf_parser.h"

#define BASE_URL "https://leaf.treelinux.org"
#define API_ENDPOINT "/api/package/"
#define DOWNLOAD_ENDPOINT "/userfiles/"

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

// Buffer for HTTP responses
typedef struct {
	char *data;
	size_t size;
} Buffer;

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
	size_t realsize = size * nmemb;
	Buffer *buf = (Buffer *)userp;
	
	char *ptr = realloc(buf->data, buf->size + realsize + 1);
	if (!ptr) {
		fprintf(stderr, "Out of memory\n");
		return 0;
	}
	
	buf->data = ptr;
	memcpy(&(buf->data[buf->size]), contents, realsize);
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

// Ensure directory exists
static int ensure_dir(const char *path) {
	struct stat st;
	if (stat(path, &st) == 0) {
		return S_ISDIR(st.st_mode) ? 0 : -1;
	}
	return mkdir(path, 0755);
}

// Get cache directory path (~/.leaf/cache)
static char *get_cache_dir(void) {
	const char *home = getenv("HOME");
	if (!home) {
		fprintf(stderr, "Cannot determine home directory\n");
		return NULL;
	}
	
	size_t len = strlen(home) + 20;
	char *cache_dir = malloc(len);
	if (!cache_dir) return NULL;
	
	snprintf(cache_dir, len, "%s/.leaf", home);
	if (ensure_dir(cache_dir) != 0) {
		fprintf(stderr, "Cannot create ~/.leaf directory\n");
		free(cache_dir);
		return NULL;
	}
	
	snprintf(cache_dir, len, "%s/.leaf/cache", home);
	if (ensure_dir(cache_dir) != 0) {
		fprintf(stderr, "Cannot create ~/.leaf/cache directory\n");
		free(cache_dir);
		return NULL;
	}
	
	return cache_dir;
}

// Get packages directory path (~/leaf/packages)
static char *get_packages_dir(void) {
	const char *home = getenv("HOME");
	if (!home) {
		fprintf(stderr, "Cannot determine home directory\n");
		return NULL;
	}
	
	size_t len = strlen(home) + 30;
	char *packages_dir = malloc(len);
	if (!packages_dir) return NULL;
	
	snprintf(packages_dir, len, "%s/leaf", home);
	if (ensure_dir(packages_dir) != 0) {
		fprintf(stderr, "Cannot create ~/leaf directory\n");
		free(packages_dir);
		return NULL;
	}
	
	snprintf(packages_dir, len, "%s/leaf/packages", home);
	if (ensure_dir(packages_dir) != 0) {
		fprintf(stderr, "Cannot create ~/leaf/packages directory\n");
		free(packages_dir);
		return NULL;
	}
	
	return packages_dir;
}

// Run a command silently and return exit code
static int run_command_silent(const char *cmd) {
	int ret = system(cmd);
	if (ret == -1) {
		return -1;
	}
	return WEXITSTATUS(ret);
}

// Check if a command exists in PATH (system-wide check)
static int command_exists(const char *cmd_name) {
	char check_cmd[512];
	snprintf(check_cmd, sizeof(check_cmd), "which '%s' >/dev/null 2>&1", cmd_name);
	return run_command_silent(check_cmd) == 0;
}

// Check if a package/dependency is installed
// Checks: 1) system PATH (bin directories), 2) ~/leaf/packages
static int is_package_installed(const char *pkg_name) {
	// First check if it exists as a system command
	if (command_exists(pkg_name)) {
		return 1;
	}
	
	// Check in ~/leaf/packages
	char *packages_dir = get_packages_dir();
	if (!packages_dir) return 0;
	
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", packages_dir, pkg_name);
	free(packages_dir);
	
	struct stat st;
	return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

// Forward declarations
static int get_terminal_width(void);
static char *get_log_path(void);
static void draw_progress(const char *msg, int frame);

// Draw a real progress bar with known percentage
static void draw_real_progress(const char *msg, const char *stage, int percent) {
	int term_width = get_terminal_width();
	
	// Calculate bar width
	int msg_len = (int)strlen(msg);
	int stage_len = stage ? (int)strlen(stage) + 3 : 0; // " - stage"
	int bar_width = term_width - 1 - msg_len - stage_len - 1 - 2 - 5; // space, brackets, percent
	if (bar_width < 10) bar_width = 10;
	
	if (percent < 0) percent = 0;
	if (percent > 100) percent = 100;
	
	int bar_fill = (percent * bar_width) / 100;
	
	printf("\r\033[K%s", msg);
	if (stage) printf(" - %s", stage);
	printf(" [");
	for (int i = 0; i < bar_width; i++) {
		if (i < bar_fill) {
			printf("\033[1;32m=\033[0m");
		} else if (i == bar_fill && percent < 100) {
			printf("\033[1;32m>\033[0m");
		} else {
			printf(" ");
		}
	}
	printf("] %3d%%", percent);
	fflush(stdout);
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
	char *log_path = get_log_path();
	if (!log_path) return -1;
	
	// Create pipe for reading git's progress
	int pipefd[2];
	if (pipe(pipefd) == -1) {
		free(log_path);
		return -1;
	}
	
	pid_t pid = fork();
	if (pid == -1) {
		close(pipefd[0]);
		close(pipefd[1]);
		free(log_path);
		return -1;
	}
	
	if (pid == 0) {
		// Child process
		close(pipefd[0]); // Close read end
		
		// Also log to file
		FILE *logf = fopen(log_path, "a");
		
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
		
		execlp("git", "git", "clone", "--progress", github_url, dest_dir, (char *)NULL);
		_exit(127);
	}
	
	// Parent process
	close(pipefd[1]); // Close write end
	
	// Open log file for appending git output
	FILE *logf = fopen(log_path, "a");
	
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
			printf("\r\033[K");
			close(pipefd[0]);
			if (logf) fclose(logf);
			free(log_path);
			return -1;
		}
		
		draw_real_progress("Cloning", stage, percent);
		struct timespec ts = {0, 100000000}; // 100ms
		nanosleep(&ts, NULL);
	}
	
	close(pipefd[0]);
	if (logf) fclose(logf);
	
	printf("\r\033[K");
	free(log_path);
	
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
		printf("No dependencies to install.\n");
		return 0;
	}
	
	printf("\nChecking %zu dependencies...\n", manifest->dependency_count);
	
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

// Get terminal width
static int get_terminal_width(void) {
	struct winsize w;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
		return w.ws_col;
	}
	return 80; // default
}

// Get log file path
static char *get_log_path(void) {
	const char *home = getenv("HOME");
	if (!home) return NULL;
	
	char *path = malloc(512);
	if (!path) return NULL;
	
	snprintf(path, 512, "%s/.leaf/log.txt", home);
	return path;
}

// Draw an indeterminate progress bar (bouncing animation - for unknown duration tasks)
static void draw_progress(const char *msg, int frame) {
	const char *spinner = "|/-\\";
	int term_width = get_terminal_width();
	
	// Calculate bar width: term_width - spinner(1) - space(1) - msg - space(1) - brackets(2) - dots(3)
	int msg_len = (int)strlen(msg);
	int bar_width = term_width - 1 - 1 - msg_len - 1 - 2 - 3;
	if (bar_width < 10) bar_width = 10;
	
	// Bouncing block animation
	int block_width = 5;
	int pos = frame % ((bar_width - block_width) * 2);
	if (pos >= bar_width - block_width) {
		pos = (bar_width - block_width) * 2 - pos;
	}
	
	// Animated dots
	int dots = (frame / 3) % 4;
	
	printf("\r\033[K%c %s [", spinner[frame % 4], msg);
	for (int i = 0; i < bar_width; i++) {
		if (i >= pos && i < pos + block_width) {
			printf("\033[1;32m=\033[0m");
		} else {
			printf(" ");
		}
	}
	printf("]");
	for (int i = 0; i < dots; i++) printf(".");
	for (int i = dots; i < 3; i++) printf(" ");
	fflush(stdout);
}

// Run compile command in package directory (silent with progress bar, log to file)
static int compile_package(const char *pkg_dir, const char *compile_cmd) {
	if (!compile_cmd || !*compile_cmd) {
		return 0;
	}
	
	char *log_path = get_log_path();
	if (!log_path) {
		fprintf(stderr, "Cannot determine log path\n");
		return -1;
	}
	
	// Fork to run command while showing progress
	pid_t pid = fork();
	if (pid == -1) {
		fprintf(stderr, "Failed to fork\n");
		free(log_path);
		return -1;
	}
	
	if (pid == 0) {
		// Child: redirect stdout/stderr to log file
		FILE *logf = fopen(log_path, "a");
		if (logf) {
			dup2(fileno(logf), STDOUT_FILENO);
			dup2(fileno(logf), STDERR_FILENO);
			fclose(logf);
		} else {
			// Fallback: redirect to /dev/null
			freopen("/dev/null", "w", stdout);
			freopen("/dev/null", "w", stderr);
		}
		
		// Change to package directory and run compile
		if (chdir(pkg_dir) != 0) {
			_exit(127);
		}
		execl("/bin/sh", "sh", "-c", compile_cmd, (char *)NULL);
		_exit(127);
	}
	
	// Parent: show progress bar while waiting
	int status;
	int frame = 0;
	while (1) {
		pid_t result = waitpid(pid, &status, WNOHANG);
		if (result == pid) {
			// Child finished
			break;
		} else if (result == -1) {
			printf("\r\033[K");
			fprintf(stderr, "waitpid error\n");
			free(log_path);
			return -1;
		}
		
		// Still running, update progress
		draw_progress("Compiling", frame++);
		struct timespec ts = {0, 100000000}; // 100ms
		nanosleep(&ts, NULL);
	}
	
	// Clear progress line
	printf("\r\033[K");
	
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		printf("\033[1;32m✓\033[0m Compilation successful!\n");
		free(log_path);
		return 0;
	} else {
		int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		fprintf(stderr, "\033[1;31m✗\033[0m Compilation failed (exit code %d)\n", exit_code);
		fprintf(stderr, "  See log: %s\n", log_path);
		free(log_path);
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
	char *cache_dir = NULL;
	char *packages_dir = NULL;
	leaf_manifest *manifest = NULL;
	char filepath[512];
	char pkg_dest[512];
	int ret = 1;
	
	// Check if already installed
	if (is_package_installed(pkg_name)) {
		printf("Package '%s' is already installed.\n", pkg_name);
		return 0;
	}
	
	curl_global_init(CURL_GLOBAL_DEFAULT);
	
	// Get directories
	cache_dir = get_cache_dir();
	if (!cache_dir) {
		goto cleanup;
	}
	
	packages_dir = get_packages_dir();
	if (!packages_dir) {
		goto cleanup;
	}
	
	// Find package in repository
	printf("\n=== Installing: %s ===\n", pkg_name);
	printf("Searching for package '%s'...\n", pkg_name);
	if (fetch_package_info(pkg_name, &username, &filename) != 0) {
		goto cleanup;
	}
	
	printf("Found: %s by %s\n", filename, username);
	
	// Download package manifest
	if (download_package(username, filename, cache_dir) != 0) {
		goto cleanup;
	}
	
	// Parse manifest
	snprintf(filepath, sizeof(filepath), "%s/%s", cache_dir, filename);
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
	snprintf(pkg_dest, sizeof(pkg_dest), "%s/%s", packages_dir, name);
	
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
	curl_global_cleanup();
	free(username);
	free(filename);
	free(cache_dir);
	free(packages_dir);
	if (manifest) free_leaf_manifest(manifest);
	return ret;
}

static int cmd_grow(const Options *o) {
	return install_package(o->pkg);
}

static int cmd_uproot(const Options *o) {
	const char *pkg_name = o->pkg;
	char *packages_dir = get_packages_dir();
	if (!packages_dir) {
		fprintf(stderr, "Cannot determine packages directory\n");
		return 1;
	}
	
	char pkg_path[512];
	snprintf(pkg_path, sizeof(pkg_path), "%s/%s", packages_dir, pkg_name);
	
	// Check if package exists in leaf packages
	struct stat st;
	if (stat(pkg_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "Package '%s' is not installed.\n", pkg_name);
		free(packages_dir);
		return 1;
	}
	
	printf("\n=== Removing: %s ===\n", pkg_name);
	printf("Location: %s\n\n", pkg_path);
	
	// Build rm -rf command (silent)
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s' 2>/dev/null", pkg_path);
	
	// Fork to run command while showing progress
	pid_t pid = fork();
	if (pid == -1) {
		fprintf(stderr, "Failed to fork\n");
		free(packages_dir);
		return 1;
	}
	
	if (pid == 0) {
		// Child: run the rm command
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	
	// Parent: show progress bar while waiting
	int status;
	int frame = 0;
	while (1) {
		pid_t result = waitpid(pid, &status, WNOHANG);
		if (result == pid) {
			// Child finished
			break;
		} else if (result == -1) {
			printf("\r\033[K");
			fprintf(stderr, "waitpid error\n");
			free(packages_dir);
			return 1;
		}
		
		// Still running, update progress
		draw_progress("Removing", frame++);
		struct timespec ts = {0, 100000000}; // 100ms
		nanosleep(&ts, NULL);
	}
	
	// Clear progress line
	printf("\r\033[K");
	
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		printf("\033[1;32m✓\033[0m Successfully removed '%s'\n", pkg_name);
		free(packages_dir);
		return 0;
	} else {
		int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		fprintf(stderr, "\033[1;31m✗\033[0m Failed to remove package (exit code %d)\n", exit_code);
		free(packages_dir);
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
