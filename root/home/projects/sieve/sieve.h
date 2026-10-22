/* sieve.h - prime sieve, the library half of the multi-file sample */
#ifndef SIEVE_H
#define SIEVE_H
/* stores every prime <= limit in primes[] (at most max); returns how many were stored */
int sieve(int limit, int *primes, int max);
#endif
