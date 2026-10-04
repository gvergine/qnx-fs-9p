/*
 * Cross-development check: write.sh builds this on the host with qcc into
 * the share, and the guest runs it from the share. It reads INPUT and
 * writes OUTPUT, both on the share, with a write, an fsync and a rename
 * over the final name.
 *
 * usage: xdev INPUT OUTPUT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "usage: xdev INPUT OUTPUT\n");
        return 2;
    }
    char line[256] = "";
    FILE *in = fopen(argv[1], "r");
    if (in == NULL || fgets(line, sizeof line, in) == NULL) {
        perror(argv[1]);
        return 1;
    }
    fclose(in);
    line[strcspn(line, "\n")] = '\0';

    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", argv[2]);
    FILE *out = fopen(tmp, "w");
    if (out == NULL) {
        perror(tmp);
        return 1;
    }
    fprintf(out, "host-built binary read: %s\n", line);
    if (fflush(out) != 0 || fsync(fileno(out)) != 0 || fclose(out) != 0 ||
        rename(tmp, argv[2]) != 0) {
        perror(argv[2]);
        return 1;
    }
    return 0;
}
