/* fortune : a random programming/OS quote */
#include <vutil.h>
#include <syscall.h>
#include <time.h>

static const char *const q[] = {
    "There are only two hard things in computer science: cache invalidation and naming things. - Phil Karlton",
    "Premature optimization is the root of all evil. - Donald Knuth",
    "Talk is cheap. Show me the code. - Linus Torvalds",
    "Simplicity is prerequisite for reliability. - Edsger Dijkstra",
    "Programs must be written for people to read, and only incidentally for machines to execute. - Abelson & Sussman",
    "Unix is simple. It just takes a genius to understand its simplicity. - Dennis Ritchie",
    "Debugging is twice as hard as writing the code in the first place. - Brian Kernighan",
    "Make it work, make it right, make it fast. - Kent Beck",
    "The best code is no code at all.",
    "A computer is a bicycle for the mind. - Steve Jobs",
    "Everything should be made as simple as possible, but no simpler. - Albert Einstein",
    "In theory, there is no difference between theory and practice. In practice, there is. - Yogi Berra",
    "It works on my machine.",
    "Real programmers count from zero.",
    "To iterate is human, to recurse divine. - L. Peter Deutsch",
    "First, solve the problem. Then, write the code. - John Johnson",
    "Any sufficiently advanced bug is indistinguishable from a feature.",
    "Weeks of coding can save you hours of planning.",
    "There is no place like 127.0.0.1.",
    "Have you tried turning it off and on again?",
    "Keep it simple, stupid.",
    "Software is eating the world, but a kernel panic is eating my afternoon.",
    "The cheapest, fastest, and most reliable components are those that aren't there. - Gordon Bell",
    "Good programmers use their brains, but good guidelines save us having to use too much of them. - Tony Hoare",
    "Perfection is achieved not when there is nothing more to add, but when there is nothing left to take away. - Saint-Exupery",
    "If it compiles, ship it. (Please don't.)",
    "Do not meddle in the affairs of kernels, for they are subtle and quick to panic.",
    "Hello, world! The first program of every OS.",
    "A journey of a thousand context switches begins with a single yield().",
    "The only way to learn a new programming language is by writing programs in it. - Dennis Ritchie",
    "Measuring programming progress by lines of code is like measuring aircraft building progress by weight. - Bill Gates",
    "UNIX was not designed to stop its users from doing stupid things, as that would also stop them from doing clever things. - Doug Gwyn",
    "Everything is a file. Except the things that aren't. Those are also files.",
    "Segmentation fault (core dumped) - a love letter from your pointer.",
    "Memory is like an orgy of bytes: nobody knows who owns what. (Sorry, memory is like a hotel: free it when you leave.)",
    "The sooner you start to code, the longer the program will take. - Roy Carlson",
    "Walking on water and developing software from a specification are easy if both are frozen. - Edward V. Berard",
    "I'm not a great programmer; I'm just a good programmer with great habits. - Kent Beck",
    "One man's constant is another man's variable. - Alan Perlis",
    "Copy-paste is a design pattern.",
    "0 errors, 0 warnings, 1 hobby OS booted. Vibes: immaculate.",
};

int main(void) {
    srand(time(NULL) ^ uptime_ms() * 2654435761u);
    puts(q[rand() % (int)(sizeof q / sizeof *q)]);
    return 0;
}
