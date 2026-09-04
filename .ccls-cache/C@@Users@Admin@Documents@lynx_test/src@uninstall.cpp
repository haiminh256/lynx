#include "uninstall.h"

#include <iostream>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

int cmd_uninstall(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "[Lynx ERROR]: Please specify a package to uninstall! Example: lynx uninstall lodash\n";
        return 1;
    }

    std::string target_package = argv[2];
    fs::path target_path = fs::current_path() / "node_modules" / target_package;

    if (fs::exists(target_path) || fs::is_symlink(target_path)) {
        std::cout << "[Lynx]: Removing package " << target_package << " from node_modules...\n";
        fs::remove_all(target_path);
        std::cout << "[Lynx]: Successfully uninstalled " << target_package << ".\n";
    } else {
        std::cerr << "[Lynx ERROR]: Package " << target_package << " is not installed in node_modules.\n";
        return 1;
    }

    return 0;
}