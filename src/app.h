#pragma once

#include <vector>
#include <string>

class CLIApp {
public:
    int run(int argc, char* argv[]);

private:
    void print_usage();
};