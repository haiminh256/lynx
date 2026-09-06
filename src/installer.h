#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <utility>

namespace fs = std::filesystem;

struct TempFileCleaner {
    fs::path filepath;
    ~TempFileCleaner() {
        std::error_code ec;
        if (!filepath.empty() && fs::exists(filepath, ec)) {
            fs::remove(filepath, ec);
        }
    }
};

class PackageInstaller {
public:
    PackageInstaller() = default;

    bool install_single_package(const std::string& raw_input);
    void install_packages_parallel(const std::vector<std::string>& targets);
    void run_all_pending_lifecycles();
    void print_summary();

private:
    std::string make_unique_temp(const std::string& package_name);
    void safe_remove(const fs::path& p);

    std::vector<std::pair<fs::path, std::string>> pending_lifecycle_packages;

    std::vector<std::string> skipped_packages;
    std::vector<std::string> installed_summary_packages;
};