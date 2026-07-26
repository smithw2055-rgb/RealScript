#include "realscript/tooling/LspServer.h"

#include <iostream>

int main() {
    realscript::tooling::LspServer server;
    return server.run(std::cin, std::cout);
}
