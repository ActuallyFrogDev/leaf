#define _POSIX_C_SOURCE 200809L
#include "leaf_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// helpers
static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *r = malloc(n);
    if (!r) return NULL;
    memcpy(r, s, n);
    return r;
}

static void trim_inplace(char *s) {
    if (!s) return;
    // trim left
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    // trim right
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
}

// extract quoted string content, handling simple escapes \" and \\.
static char *unquote_and_unescape(const char *src) {
    if (!src) return NULL;
    const char *s = src;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '"') {
        s++;
        size_t cap = 64;
        char *out = malloc(cap);
        if (!out) return NULL;
        size_t len = 0;
        while (*s) {
            if (*s == '\\' && (s[1] == '\\' || s[1] == '"')) {
                if (len + 1 >= cap) {
                    cap *= 2;
                    char *tmp = realloc(out, cap);
                    if (!tmp) {
                        free(out);
                        return NULL;
                    }
                    out = tmp;
                }
                out[len++] = s[1];
                s += 2;
                continue;
            }
            if (*s == '"') break;
            if (len + 1 >= cap) {
                cap *= 2;
                char *tmp = realloc(out, cap);
                if (!tmp) {
                    free(out);
                    return NULL;
                }
                out = tmp;
            }
            out[len++] = *s++;
        }
        // Check for malformed quoted value (missing closing quote)
        if (*s != '"') {
            free(out);
            return NULL;
        }
        out[len] = '\0';
        return out;
    }
    // not quoted: copy trimmed remainder
    char *tmp = xstrdup(src);
    if (!tmp) return NULL;
    trim_inplace(tmp);
    return tmp;
}

leaf_manifest *parse_leaf_file(const char *path) {
    if (!path) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    char *line = NULL;
    size_t len = 0;
    ssize_t read;

    leaf_manifest m = {0};

    while ((read = getline(&line, &len, f)) != -1) {
        if (read <= 0) continue;
        // strip newline
        while (read > 0 && (line[read-1] == '\n' || line[read-1] == '\r')) { line[--read] = '\0'; }
        // skip empty lines
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) continue;
        // skip comment lines (# or // or ;)
        if (*p == '#' || *p == ';') continue;
        if (*p == '/' && p[1] == '/') continue;

        // find '='
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        trim_inplace(key);
        trim_inplace(val);

        char *parsed = unquote_and_unescape(val);
        if (!parsed) continue;

        if (strcmp(key, "PACKAGE.NAME") == 0) {
            free(m.name); m.name = parsed;
        } else if (strcmp(key, "PACKAGE.VERSION") == 0) {
            free(m.version); m.version = parsed;
        } else if (strcmp(key, "PACKAGE.DESCRIPTION") == 0) {
            free(m.description); m.description = parsed;
        } else if (strcmp(key, "PACKAGE.AUTHOR") == 0) {
            free(m.author); m.author = parsed;
        } else if (strcmp(key, "PACKAGE.LICENSE") == 0) {
            free(m.license); m.license = parsed;
        } else if (strcmp(key, "PACKAGE.DEPENDENCIES") == 0) {
            free(m.dependencies_raw); m.dependencies_raw = parsed;
        } else if (strcmp(key, "PACKAGE.GITHUB") == 0) {
            free(m.github); m.github = parsed;
        } else if (strcmp(key, "PACKAGE.HOMEPAGE") == 0) {
            free(m.homepage); m.homepage = parsed;
        } else if (strcmp(key, "PACKAGE.COMPILE") == 0) {
            free(m.compile_cmd); m.compile_cmd = parsed;
        } else {
            // unknown key: ignore
            free(parsed);
        }
    }

    free(line);
    fclose(f);

    // Parse dependencies into list
    if (m.dependencies_raw) {
        char *deps_copy = xstrdup(m.dependencies_raw);
        if (deps_copy) {
            char *saveptr;
            char *token = strtok_r(deps_copy, ",", &saveptr);
            while (token && m.dependency_count < MAX_DEPENDENCIES) {
                trim_inplace(token);
                if (*token) {
                    m.dependencies[m.dependency_count] = xstrdup(token);
                    if (m.dependencies[m.dependency_count]) {
                        m.dependency_count++;
                    }
                }
                token = strtok_r(NULL, ",", &saveptr);
            }
            free(deps_copy);
        }
    }

    // allocate result
    leaf_manifest *res = malloc(sizeof(leaf_manifest));
    if (!res) {
        free(m.name); free(m.version); free(m.description);
        free(m.author); free(m.license); free(m.dependencies_raw);
        free(m.github); free(m.homepage); free(m.compile_cmd);
        for (size_t i = 0; i < m.dependency_count; i++) {
            free(m.dependencies[i]);
        }
        return NULL;
    }
    *res = m;
    return res;
}

void free_leaf_manifest(leaf_manifest *m) {
    if (!m) return;
    free(m->name);
    free(m->version);
    free(m->description);
    free(m->author);
    free(m->license);
    free(m->dependencies_raw);
    free(m->github);
    free(m->homepage);
    free(m->compile_cmd);
    for (size_t i = 0; i < m->dependency_count; i++) {
        free(m->dependencies[i]);
    }
    free(m);
}

void print_leaf_manifest(const leaf_manifest *m) {
    if (!m) {
        printf("(null manifest)\n");
        return;
    }
    
    printf("=== Leaf Package Manifest ===\n");
    printf("Name:         %s\n", m->name ? m->name : "(not set)");
    printf("Version:      %s\n", m->version ? m->version : "(not set)");
    printf("Description:  %s\n", m->description ? m->description : "(not set)");
    printf("Author:       %s\n", m->author ? m->author : "(not set)");
    printf("License:      %s\n", m->license ? m->license : "(not set)");
    printf("GitHub:       %s\n", m->github ? m->github : "(not set)");
    printf("Homepage:     %s\n", m->homepage ? m->homepage : "(not set)");
    printf("Compile:      %s\n", m->compile_cmd ? m->compile_cmd : "(not set)");
    
    printf("Dependencies: ");
    if (m->dependency_count == 0) {
        printf("(none)\n");
    } else {
        printf("(%zu)\n", m->dependency_count);
        for (size_t i = 0; i < m->dependency_count; i++) {
            printf("  - %s\n", m->dependencies[i]);
        }
    }
    printf("=============================\n");
}
