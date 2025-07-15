#include "../drivers/graphics.h"
#include "count.h"
#include "../drivers/console.h"
#include "command_utils.h"
extern volatile unsigned int timer_irq_count;
extern volatile unsigned int key_irq_count;

void cmd_count(const char* args, const char* flag) {
    (void)args;
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(rgb16(0,255,128), rgb16(0,0,0)); // bright green for usage
        print("Usage: count\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0)); // soft white for description
        print("Shows the number of timer and keyboard interrupts.\n");
        return;
    }
    graphics_console_set_color(rgb16(255,255,0), rgb16(0,0,0)); // yellow for output
    print("Timer IRQs: ");
    print_hex(timer_irq_count);
    print("\nKeyboard IRQs: ");
    print_hex(key_irq_count);
    print("\n");
    graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0));
} 