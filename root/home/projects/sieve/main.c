/* multi-file sample:  cd sieve; tcc main.c sieve.c -o sieve && ./sieve 100 */
#include <stdio.h>
#include <stdlib.h>
#include "sieve.h"

int main(int argc, char **argv) {
    int limit = argc > 1 ? atoi(argv[1]) : 50;
    int primes[10000];
    int n = sieve(limit, primes, 10000);
    printf("%d primes <= %d:", n, limit);
    for (int i = 0; i < n; i++) printf(" %d", primes[i]);
    printf("\n");
    return 0;
}
