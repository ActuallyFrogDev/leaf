#include <stdio.h>
#include <stdlib.h>
#include "leaf_parser.h"

int main(int argc, char **argv) {
    const char *path = "web/storage/example/example.leaf";
    if (argc > 1) path = argv[1];

    leaf_manifest *m = parse_leaf_file(path);
    if (!m) {
        fprintf(stderr, "Failed to parse %s\n", path);
        return 2;
    }

    printf("name: %s\n", m->name ? m->name : "(null)");
    printf("version: %s\n", m->version ? m->version : "(null)");
    printf("dependencies: %s\n", m->dependencies ? m->dependencies : "(null)");
    printf("github: %s\n", m->github ? m->github : "(null)");
    printf("compile: %s\n", m->compile_cmd ? m->compile_cmd : "(null)");

    free_leaf_manifest(m);
    return 0;
}
