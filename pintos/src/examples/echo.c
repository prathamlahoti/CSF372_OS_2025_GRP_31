#include <syscall.h>

int my_strlen(const char* s) {
    int len = 0;
    while (s[len] != '\0') len++;
    return len;
}

char* unquote(char* s) {
    int len = my_strlen(s);
    if (len >= 2 && s[0] == '\'' && s[len - 1] == '\'') {
        s[len - 1] = '\0';
        return s + 1;
    }
    return s;
}

int main(int argc, char **argv) {
    int i;
    if (argc <= 1) {
        return EXIT_SUCCESS;
    }
    for (i = 1; i < argc; i++) {
        char* arg = unquote(argv[i]);
        write(1, arg, my_strlen(arg));
        write(1, " ", 1);
    }
    write(1, "\n", 1);
    return EXIT_SUCCESS;
}