#ifndef TABLE_H
#define TABLE_H
struct entry { char *word; int count; };
void table_add(const char *word);
struct entry *table_sorted(int *n);   /* most frequent first, then alphabetical */
#endif
