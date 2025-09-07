#include <stdio.h>
#include <syscall.h>

int main(int argc, char *argv[]) {
    int i;

    if (argc < 2) {
        return 0;
    }

    for (i = 1; i < argc; i++) {
        /* Try opening the file */
        int fd = open(argv[i]);
        if (fd >= 0) {
            /* File already exists → just close it */
            close(fd);
        } else {
            /* File does not exist → create it */
            if (create(argv[i], 0)) {
                printf("%s: created\n", argv[i]);
            }
        }
    }

    return 0;
}