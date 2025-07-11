#include "exceptions.h"
#include "console.h"

const char* exception_messages[32] = {
    "#DE Divide by Zero",                // 0
    "#DB Debug",                          // 1
    "NMI Interrupt",                      // 2
    "#BP Breakpoint",                     // 3
    "#OF Overflow",                       // 4
    "#BR Bound Range Exceeded",           // 5
    "#UD Invalid Opcode",                 // 6
    "#NM Device Not Available",           // 7
    "#DF Double Fault",                   // 8
    "Coprocessor Segment Overrun",        // 9 (obsolete)
    "#TS Invalid TSS",                    // 10
    "#NP Segment Not Present",            // 11
    "#SS Stack-Segment Fault",            // 12
    "#GP General Protection Fault",       // 13
    "#PF Page Fault",                     // 14
    "Reserved",                           // 15
    "#MF x87 FPU Floating-Point Error",   // 16
    "#AC Alignment Check",                // 17
    "#MC Machine Check",                  // 18
    "#XF SIMD Floating-Point Exception",  // 19
    "#VE Virtualization Exception",       // 20
    "Control Protection Exception",       // 21
    "Reserved",                           // 22
    "Reserved",                           // 23
    "Reserved",                           // 24
    "Reserved",                           // 25
    "Reserved",                           // 26
    "Reserved",                           // 27
    "Reserved",                           // 28
    "Reserved",                           // 29
    "Reserved",                           // 30
    "Reserved"                            // 31
};

void exception_handler(int num) {
    clear_screen();
    print("CPU Exception! #");
    print_hex(num);
    print(": ");
    if (num >= 0 && num < 32) {
        print(exception_messages[num]);
    } else {
        print("Unknown");
    }
    print("\nSystem halted.\n");
    while (1) {}
} 