#include "count.h"
#include "../drivers/console.h"
#include "command_utils.h"
extern volatile unsigned int timer_irq_count;
extern volatile unsigned int key_irq_count;

void cmd_count(const char* args, const char* flag) {
    (void)args;
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        print("Usage: count [--help]\nShows the number of timer and keyboard interrupts.\n");
        return;
    }
    print("Timer IRQs: ");
    print_hex(timer_irq_count);
    print("\nKeyboard IRQs: ");
    print_hex(key_irq_count);
    print("\n");
} 