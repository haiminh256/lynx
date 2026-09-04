#include <iostream>
#include <string>

#include "install.h"
#include "run.h"
#include "uninstall.h"
#include "create.h"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "[Lynx]: Usage:\n"
                  << "  lynx install\n"
                  << "  lynx install [package_name]\n"
                  << "  lynx uninstall [package_name]\n"
                  << "  lynx create [template][@version] [args...]\n"
                  << "  lynx run                  (list scripts)\n"
                  << "  lynx run [script_name]    (run independently)\n";
        return 1;
    }

    std::string command = argv[1];

    if (command == "install") {
        return cmd_install(argc, argv);
    } else if (command == "uninstall") {
        return cmd_uninstall(argc, argv);
    } else if (command == "run") {
        return cmd_run(argc, argv);
    } else if (command == "create") {
        return cmd_create(argc, argv);
    } else {
        std::cerr << "[Lynx ERROR]: Unknown command \"" << command << "\"\n";
        return 1;
    }

    return 0;
}