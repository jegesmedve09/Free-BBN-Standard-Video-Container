#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        printf("Usage: %s <input> <output>\n", argv[0]);
        return 1;
    }

    FILE *in = fopen(argv[1], "rb");
    if (!in)
    {
        perror("Failed to open input");
        return 1;
    }

    FILE *out = fopen(argv[2], "wb");
    if (!out)
    {
        perror("Failed to open output");
        fclose(in);
        return 1;
    }

    int b1, b2;

    while ((b1 = fgetc(in)) != EOF)
    {
        fputc(b1, out);

        // Skip the second byte
        b2 = fgetc(in);
        if (b2 == EOF)
            break;
    }

    fclose(in);
    fclose(out);

    printf("Done.\n");
    return 0;
}
