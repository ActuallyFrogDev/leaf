#ifndef LEAF_PARSER_H
#define LEAF_PARSER_H

#include <stddef.h>

typedef struct leaf_manifest {
    char *name;
    char *version;
    char *dependencies;
    char *github;
    char *compile_cmd;
} leaf_manifest;

// Parse a .leaf file at `path`. Returns a newly-allocated leaf_manifest
// (caller must free with free_leaf_manifest). Returns NULL on error.
leaf_manifest *parse_leaf_file(const char *path);

// Free a manifest returned by parse_leaf_file.
void free_leaf_manifest(leaf_manifest *m);

#endif // LEAF_PARSER_H
