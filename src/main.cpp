#include <iostream>

#include "usi_engine.h"

int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    shogi::UsiEngine engine;
    engine.loop();
    return 0;
}
