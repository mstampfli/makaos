#include "libc.h"
#include "stdio.h"

int main(int argc, char** argv) {
    const char* path = ".";
    if (argc > 1) path = argv[1];

    // Resolve "." to actual cwd if needed
    char cwdbuf[512];
    const char* dirpath = path;
    if (path[0] == '.' && path[1] == '\0') {
        char* cwd = getcwd(NULL, 0);
        if (cwd) {
            dirpath = cwd;
        }
    }

    DIR* dir = opendir(dirpath);
    if (!dir) {
        fprintf(stderr, "ls: cannot open '%s'\n", dirpath);
        return 1;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            continue;
        printf("%s\n", entry->d_name);
    }
    closedir(dir);
    return 0;
}
