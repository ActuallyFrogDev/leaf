#ifndef LEAF_PARSER_H
#define LEAF_PARSER_H

#include <stddef.h>

#define MAX_DEPENDENCIES 64

typedef struct leaf_manifest {
    // Core package info
    char *name;
    char *version;
    char *description;
    char *author;
    char *license;
    char *github;
    char *homepage;
    char *compile_cmd;
    
    // Dependencies as a list
    char *dependencies_raw;           // Original raw string
    char *dependencies[MAX_DEPENDENCIES];  // Parsed list
    size_t dependency_count;
} leaf_manifest;

// Parse a .leaf file at `path`. Returns a newly-allocated leaf_manifest
// (caller must free with free_leaf_manifest). Returns NULL on error.
leaf_manifest *parse_leaf_file(const char *path);

// Free a manifest returned by parse_leaf_file.
void free_leaf_manifest(leaf_manifest *m);

// Print all manifest fields to stdout
void print_leaf_manifest(const leaf_manifest *m);

#endif // LEAF_PARSER_H
