#pragma once

#include "types.hh"

class Plic {
public:

    static void init();

    static void init_hart();

    static int claim();

    static void complete(int irq);
};
