#ifndef COMMAND_UTILS_H
#define COMMAND_UTILS_H

// Minimal kernel string compare (like strcmp)
static inline int kstrcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}
// Minimal kernel string compare for n chars (like strncmp)
static inline int kstrncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i] || !b[i]) return a[i] - b[i];
    }
    return 0;
}

#endif // COMMAND_UTILS_H 