#include <stdlib.h>
#include "sieve.h"

int sieve(int limit, int *primes, int max) {
    char *composite = calloc(limit + 1, 1);
    int n = 0;
    for (int i = 2; i <= limit; i++) {
        if (composite[i]) continue;
        if (n < max) primes[n++] = i;
        for (long j = (long)i * i; j <= limit; j += i) composite[j] = 1;
    }
    free(composite);
    return n;
}
