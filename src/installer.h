#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <utility>
#include <map>
#include <set>

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

struct InstallContext {
    bool is_direct = true;
    // Khi true: cho phép ghi đè version đã có (dùng cho dependency
    // của một direct package đang được cài/upgrade).
    bool allow_overwrite = false;
    std::vector<std::string> chain;
};

class PackageInstaller {
public:
    PackageInstaller() = default;

    bool install_single_package(const std::string& raw_input, bool is_global = false,
                                const InstallContext& ctx = {});
    void install_packages_parallel(const std::vector<std::string>& targets, bool is_global = false,
                                   const InstallContext& ctx = {});
    void run_all_pending_lifecycles();
    void print_summary();
    void clear_summary();

private:
    std::string make_unique_temp(const std::string& package_name);
    void safe_remove(const fs::path& p);

    std::vector<std::pair<fs::path, std::string>> pending_lifecycle_packages;
    std::vector<std::string> skipped_packages;
    std::vector<std::string> installed_summary_packages;

    std::set<std::string> in_progress_targets;

    struct ClaimGuard {
        PackageInstaller* self;
        std::string key;
        bool active;
        ClaimGuard(PackageInstaller* s, std::string k) : self(s), key(std::move(k)), active(true) {}
        ClaimGuard(const ClaimGuard&) = delete;
        ClaimGuard& operator=(const ClaimGuard&) = delete;
        ~ClaimGuard();
    };
};