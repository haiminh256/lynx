#pragma once

#include <string>
#include <vector>

bool install_single_package(const std::string& raw_input);
void install_packages_parallel(const std::vector<std::string>& targets);
int cmd_install(int argc, char* argv[]);