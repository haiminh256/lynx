#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <set>
#include <mutex>

namespace fs = std::filesystem;

extern std::set<std::string> installed_packages;
extern std::mutex install_mutex;

constexpr int LYNX_MAX_PARALLEL = 30;

fs::path get_lynx_cache_dir();

// Thêm các hàm hỗ trợ Global
fs::path get_global_dir();
fs::path get_global_bin_dir();

std::string sanitize_filename(std::string name);
void generate_bin_shims(const fs::path& package_path, const std::string& package_name, const fs::path& custom_bin_dir = {});

bool run_lifecycle_scripts(const fs::path& package_path, const std::string& package_name, bool is_root = false);

// Quotes a string so it can be safely embedded as a single argument in a
// shell command line built via std::system()/cmd.exe. This is defense in
// depth for values that originate from network responses (tarball URLs,
// package names) rather than a fully-trusted source. POSIX quoting (single
// quotes) is airtight; the Windows cmd.exe quoting is best-effort since
// cmd has no fully safe quoting mode — prefer argv-based process spawning
// (CreateProcess with an argument vector, not a composed command line)
// wherever possible instead of relying on this alone.
std::string shell_quote(const std::string& s);

struct CasFile {
    std::string relative_path;
    std::string hash;
};

std::string sha256_file(const fs::path& path);

fs::path add_to_cas(const fs::path& source_file);

bool import_package_to_cas(const fs::path& extracted_dir,
                           const std::string& pkg_name,
                           const std::string& version);

bool is_package_in_cas(const std::string& pkg_name, const std::string& version);

bool materialize_from_cas(const std::string& pkg_name,
                          const std::string& version,
                          const fs::path& target_dir);

bool hardlink_directory(const fs::path& from, const fs::path& to);
bool link_or_copy_directory(const fs::path& from, const fs::path& to);