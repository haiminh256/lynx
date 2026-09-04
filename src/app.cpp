#include "app.h"
#include "commands.h"
#include <iostream>

void CLIApp::print_usage() {
    std::cout << "[Lynx CLI]: Usage:\n"
              << "  lynx install [package_name]\n"
              << "  lynx uninstall <package_name>\n"
              << "  lynx run [script_name]\n"
              << "  lynx create <template>[@version]\n";
}

int CLIApp::run(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command_name = argv[1];
    auto command = CommandFactory::create_command(command_name);

    if (!command) {
        std::cerr << "[Lynx ERROR]: Unknown command \"" << command_name << "\"\n";
        return 1;
    }

    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) {
        args.push_back(argv[i]);
    }

    return command->execute(args);
}