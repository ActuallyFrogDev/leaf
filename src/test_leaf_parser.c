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

    print_leaf_manifest(m);

    free_leaf_manifest(m);
    return 0;
}
