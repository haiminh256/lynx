#include "commands.h"
#include "installer.h"
#include "lockfile.h"
#include "utils.h"
#include "json.hpp"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <cstdlib>

using json = nlohmann::json;
namespace fs = std::filesystem;
#ifdef _WIN32
#include <windows.h>

int run_command_no_batch(const std::string& cmd) {
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi{};

    std::string full_cmd = "cmd.exe /d /s /c \"" + cmd + "\"";

    BOOL ok = CreateProcessA(
        nullptr,
        full_cmd.data(),
        nullptr, nullptr,
        TRUE,
        CREATE_NEW_PROCESS_GROUP,
        nullptr,
        nullptr,
        &si, &pi
    );

    if (!ok) {
        DWORD err = GetLastError();
        std::cerr << "[Lynx ERROR]: CreateProcess failed (" << err << ")\n";
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return static_cast<int>(exit_code);
}
#endif

int InstallCommand::execute(const std::vector<std::string>& args) {
    PackageInstaller installer;
    bool is_dev = false;
    std::vector<std::string> target_packages;

    for (const auto& arg : args) {
        if (arg == "-D" || arg == "--save-dev") {
            is_dev = true;
        } else {
            target_packages.push_back(arg);
        }
    }

    if (target_packages.empty()) {
        std::string pkg_json_path = "package.json";
        if (!fs::exists(pkg_json_path)) {
            std::cerr << "[Lynx ERROR]: No package.json found in current directory!\n";
            return 1;
        }

        std::ifstream file(pkg_json_path);
        json pkg_json;
        try {
            file >> pkg_json;
            file.close();
        } catch (...) {
            std::cerr << "[Lynx ERROR]: Invalid package.json format!\n";
            return 1;
        }

        std::cout << "[Lynx]: Found package.json. Scanning dependencies...\n";
        std::vector<std::string> all_targets;

        const std::vector<std::string> dep_keys = {
            "dependencies", "devDependencies", "peerDependencies", "optionalDependencies"
        };

        for (const auto& key : dep_keys) {
            if (pkg_json.contains(key) && !pkg_json[key].empty()) {
                for (auto& [name, version] : pkg_json[key].items()) {
                    all_targets.push_back(name + "@" + version.get<std::string>());
                }
            }
        }

        if (all_targets.empty()) {
            std::cout << "[Lynx]: No dependencies found to install.\n";
        } else {
            std::cout << "[Lynx]: Installing " << all_targets.size() << " packages...\n\n";
            installer.install_packages_parallel(all_targets);
            std::cout << "\n[Lynx]: All packages installed.\n";
        }
        run_lifecycle_scripts(fs::current_path(), "root_project");
    } else {
        if (target_packages.size() == 1) {
            installer.install_single_package(target_packages[0]);
        } else {
            installer.install_packages_parallel(target_packages);
        }

        std::string pkg_json_path = "package.json";
        if (fs::exists(pkg_json_path)) {
            std::ifstream file(pkg_json_path);
            json pkg_json;
            try {
                file >> pkg_json;
                file.close();

                std::string target_section = is_dev ? "devDependencies" : "dependencies";

                for (const auto& raw_pkg : target_packages) {
                    std::string pkg_name = raw_pkg;
                    std::string pkg_ver = "^latest";

                    size_t at_pos = raw_pkg.find('@');
                    if (at_pos == 0) at_pos = raw_pkg.find('@', 1);

                    if (at_pos != std::string::npos && at_pos > 0) {
                        pkg_name = raw_pkg.substr(0, at_pos);
                        pkg_ver = "^" + raw_pkg.substr(at_pos + 1);
                    }

                    if (g_lockfile.has_package(pkg_name, "")) {
                        std::map<std::string, std::string> deps = g_lockfile.get_dependencies(pkg_name);
                    }

                    pkg_json[target_section][pkg_name] = pkg_ver;
                }

                std::ofstream out_file(pkg_json_path);
                out_file << pkg_json.dump(2) << std::endl;
                out_file.close();
                std::cout << "[Lynx]: Saved to " << target_section << " in package.json\n";
            } catch (...) {
                std::cerr << "[Lynx WARNING]: Failed to update package.json\n";
            }
        }
    }

    g_lockfile.save();
    std::cout << "[Lynx]: Updated lynx-lock.json\n";
    return 0;
}
int UninstallCommand::execute(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "[Lynx ERROR]: Please specify a package to uninstall!\n";
        return 1;
    }

    std::string target_package = args[0];
    fs::path target_path = fs::current_path() / "node_modules" / target_package;

    if (fs::exists(target_path) || fs::is_symlink(target_path)) {
        std::cout << "[Lynx]: Removing " << target_package << "...\n";
        fs::remove_all(target_path);

        g_lockfile.remove_package(target_package);
        g_lockfile.save();

        std::cout << "[Lynx]: Successfully uninstalled " << target_package << ".\n";
    } else {
        std::cerr << "[Lynx ERROR]: Package " << target_package << " is not installed.\n";
        return 1;
    }
    return 0;
}

int RunCommand::execute(const std::vector<std::string>& args) {
    std::string pkg_json_path = "package.json";
    if (!fs::exists(pkg_json_path)) {
        std::cerr << "[Lynx ERROR]: No package.json found!\n";
        return 1;
    }

    std::ifstream file(pkg_json_path);
    json pkg_json;
    try {
        file >> pkg_json;
    } catch (...) {
        std::cerr << "[Lynx ERROR]: Failed to parse package.json!\n";
        return 1;
    }

    if (args.empty()) {
        if (!pkg_json.contains("scripts") || pkg_json["scripts"].empty()) {
            std::cout << "[Lynx]: No scripts found in package.json.\n";
            return 0;
        }
        std::cout << "[Lynx]: Available scripts:\n\n";
        for (auto& [name, cmd] : pkg_json["scripts"].items()) {
            std::cout << "  " << name << "\n    " << cmd.get<std::string>() << "\n\n";
        }
        return 0;
    }

    std::string script_name = args[0];
    if (!pkg_json.contains("scripts") || !pkg_json["scripts"].contains(script_name)) {
        std::cerr << "[Lynx ERROR]: Script \"" << script_name << "\" not found!\n";
        return 1;
    }

    std::string script_cmd = pkg_json["scripts"][script_name].get<std::string>();
    for (size_t i = 1; i < args.size(); ++i) {
        script_cmd += " " + args[i];
    }

    fs::path bin_dir = fs::current_path() / "node_modules" / ".bin";
    const char* old_path_c = std::getenv("PATH");
    std::string old_path = old_path_c ? old_path_c : "";

#ifdef _WIN32
    std::string path_for_child = bin_dir.string() + ";" + old_path;
    _putenv_s("PATH", path_for_child.c_str());
    int ret = run_command_no_batch(script_cmd);
    _putenv_s("PATH", old_path.c_str());
    return ret;
#else
    std::string path_for_child = bin_dir.string() + ":" + old_path;
    setenv("PATH", path_for_child.c_str(), 1);
    int ret = std::system(script_cmd.c_str());
    setenv("PATH", old_path.c_str(), 1);
    return ret;
#endif
}

int CreateCommand::execute(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "[Lynx ERROR]: Usage: lynx create <template>[@version]\n";
        return 1;
    }

    std::string raw_input = args[0];
    std::string pkg_name = raw_input;
    std::string version = "";

    size_t at_pos = raw_input.find('@');
    if (at_pos == 0) {
        at_pos = raw_input.find('@', 1);
    }
    if (at_pos != std::string::npos && at_pos > 0) {
        pkg_name = raw_input.substr(0, at_pos);
        version = raw_input.substr(at_pos);
    }

    if (pkg_name.rfind("create-", 0) != 0) {
        pkg_name = "create-" + pkg_name;
    }

    std::string full_install_pkg = pkg_name + version;

    PackageInstaller installer;
    std::cout << "[Lynx]: Installing creator package " << full_install_pkg << "...\n";
    if (!installer.install_single_package(full_install_pkg)) {
        return 1;
    }

    fs::path bin_dir = fs::current_path() / "node_modules" / ".bin";

    std::string bin_name = pkg_name; 

    std::string run_cmd = bin_name;
    for (size_t i = 1; i < args.size(); ++i) {
        run_cmd += " " + args[i];
    }

    const char* old_path_c = std::getenv("PATH");
    std::string old_path = old_path_c ? old_path_c : "";

#ifdef _WIN32
    std::string path_for_child = bin_dir.string() + ";" + old_path;
    _putenv_s("PATH", path_for_child.c_str());
    int ret = run_command_no_batch(run_cmd);
    _putenv_s("PATH", old_path.c_str());
    return ret;
#else
    std::string path_for_child = bin_dir.string() + ":" + old_path;
    setenv("PATH", path_for_child.c_str(), 1);
    int ret = std::system(run_cmd.c_str());
    setenv("PATH", old_path.c_str(), 1);
    return ret;
#endif
}

std::unique_ptr<ICommand> CommandFactory::create_command(const std::string& name) {
    if (name == "install") return std::make_unique<InstallCommand>();
    if (name == "uninstall") return std::make_unique<UninstallCommand>();
    if (name == "run") return std::make_unique<RunCommand>();
    if (name == "create") return std::make_unique<CreateCommand>();
    return nullptr;
}
