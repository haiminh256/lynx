#include "installer.h"
#include "utils.h"
#include "lockfile.h"
#include "json.hpp"

#include <iostream>
#include <fstream>
#include <cstdlib>
#include <thread>
#include <future>
#include <chrono>
#include <atomic>
#include <sstream>
#include <system_error>
#include <vector>
#include <algorithm>

using json = nlohmann::json;

static std::atomic<uint64_t> g_temp_counter{0};

PackageInstaller::ClaimGuard::~ClaimGuard() {
    if (active) {
        std::lock_guard<std::mutex> lock(install_mutex);
        self->in_progress_targets.erase(key);
    }
}

struct SemVer {
    int major = 0, minor = 0, patch = 0;
    bool is_prerelease = false;
    std::string prerelease_tag;

    static SemVer parse(const std::string& v_str) {
        SemVer sv;
        std::string clean = v_str;
        while (!clean.empty() && std::string("^~=><").find(clean.front()) != std::string::npos) {
            clean.erase(0, 1);
        }
        size_t dash = clean.find('-');
        if (dash != std::string::npos) {
            sv.is_prerelease = true;
            sv.prerelease_tag = clean.substr(dash + 1);
            clean = clean.substr(0, dash);
        }
        std::stringstream ss(clean);
        std::string part;
        if (std::getline(ss, part, '.')) try { sv.major = std::stoi(part); } catch (...) {}
        if (std::getline(ss, part, '.')) try { sv.minor = std::stoi(part); } catch (...) {}
        if (std::getline(ss, part, '.')) try { sv.patch = std::stoi(part); } catch (...) {}
        return sv;
    }

    bool operator>(const SemVer& o) const {
        if (major != o.major) return major > o.major;
        if (minor != o.minor) return minor > o.minor;
        if (patch != o.patch) return patch > o.patch;
        if (is_prerelease && o.is_prerelease) return prerelease_tag > o.prerelease_tag;
        return !is_prerelease && o.is_prerelease;
    }
    bool operator>=(const SemVer& o) const { return *this > o || *this == o; }
    bool operator==(const SemVer& o) const {
        return major == o.major && minor == o.minor && patch == o.patch &&
               is_prerelease == o.is_prerelease && prerelease_tag == o.prerelease_tag;
    }
};

static std::string resolve_best_version(const json& parsed_data, const std::string& range_req) {
    if (!parsed_data.contains("versions") || parsed_data["versions"].empty()) return "";

    std::string range = range_req;
    range.erase(std::remove_if(range.begin(), range.end(), ::isspace), range.end());

    if (range.empty() || range == "*" || range == "latest" || range == "x" || range == "X") {
        if (parsed_data.contains("dist-tags") && parsed_data["dist-tags"].contains("latest")) {
            return parsed_data["dist-tags"]["latest"].get<std::string>();
        }
    }
    if (parsed_data["versions"].contains(range)) return range;

    std::string op = "^";
    std::string clean = range;
    if (clean.rfind(">=", 0) == 0)      { op = ">="; clean = clean.substr(2); }
    else if (clean.rfind("<=", 0) == 0) { op = "<="; clean = clean.substr(2); }
    else if (clean.rfind(">", 0) == 0)  { op = ">";  clean = clean.substr(1); }
    else if (clean.rfind("<", 0) == 0)  { op = "<";  clean = clean.substr(1); }
    else if (!clean.empty() && (clean[0]=='^'||clean[0]=='~'||clean[0]=='=')) {
        op = std::string(1, clean[0]);
        clean = clean.substr(1);
    }

    SemVer target = SemVer::parse(clean);
    bool want_pre = target.is_prerelease || (range.find('-') != std::string::npos);

    std::string best;
    SemVer best_sv{-1,-1,-1,true};

    for (auto it = parsed_data["versions"].begin(); it != parsed_data["versions"].end(); ++it) {
        SemVer curr = SemVer::parse(it.key());
        if (curr.is_prerelease && !want_pre) continue;

        bool ok = false;
        if (op == "^") ok = (curr.major == target.major && curr >= target);
        else if (op == "~") ok = (curr.major == target.major && curr.minor == target.minor && curr.patch >= target.patch);
        else if (op == "=") ok = (curr == target);
        else if (op == ">=") ok = (curr >= target);
        else if (op == ">")  ok = (curr > target);
        else if (op == "<=") ok = !(curr > target);
        else if (op == "<")  ok = (target > curr);

        if (ok && (best.empty() || curr > best_sv)) {
            best_sv = curr;
            best = it.key();
        }
    }
    if (!best.empty()) return best;

    for (auto it = parsed_data["versions"].rbegin(); it != parsed_data["versions"].rend(); ++it) {
        SemVer curr = SemVer::parse(it.key());
        if (curr.is_prerelease && !want_pre) continue;
        return it.key();
    }
    return parsed_data["versions"].rbegin().key();
}

static bool version_satisfies_range(const std::string& version_str, const std::string& range_req) {
    std::string range = range_req;
    range.erase(std::remove_if(range.begin(), range.end(), ::isspace), range.end());

    if (range.empty() || range == "*" || range == "latest" || range == "x" || range == "X") return true;
    if (range == version_str) return true;

    std::string op = "^";
    std::string clean = range;
    if (clean.rfind(">=", 0) == 0)      { op = ">="; clean = clean.substr(2); }
    else if (clean.rfind("<=", 0) == 0) { op = "<="; clean = clean.substr(2); }
    else if (clean.rfind(">", 0) == 0)  { op = ">";  clean = clean.substr(1); }
    else if (clean.rfind("<", 0) == 0)  { op = "<";  clean = clean.substr(1); }
    else if (!clean.empty() && (clean[0]=='^'||clean[0]=='~'||clean[0]=='=')) {
        op = std::string(1, clean[0]);
        clean = clean.substr(1);
    }

    SemVer target = SemVer::parse(clean);
    SemVer curr = SemVer::parse(version_str);

    bool want_pre = target.is_prerelease || (range.find('-') != std::string::npos);
    if (curr.is_prerelease && !want_pre) return false;

    if (op == "^")  return curr.major == target.major && curr >= target;
    if (op == "~")  return curr.major == target.major && curr.minor == target.minor && curr.patch >= target.patch;
    if (op == "=")  return curr == target;
    if (op == ">=") return curr >= target;
    if (op == ">")  return curr > target;
    if (op == "<=") return !(curr > target);
    if (op == "<")  return target > curr;
    return false;
}

static std::string read_installed_version(const fs::path& package_dir) {
    std::error_code ec;
    fs::path pj = package_dir / "package.json";
    if (!fs::exists(pj, ec)) return "";

    std::ifstream f(pj);
    if (!f) return "";
    try {
        json j;
        f >> j;
        if (j.contains("version") && j["version"].is_string()) {
            return j["version"].get<std::string>();
        }
    } catch (...) {}
    return "";
}

std::string PackageInstaller::make_unique_temp(const std::string& package_name) {
    uint64_t n = g_temp_counter.fetch_add(1);
    std::ostringstream oss;
    oss << "lynx_meta_" << sanitize_filename(package_name) << "_" << n << "_"
        << std::hash<std::thread::id>{}(std::this_thread::get_id()) << ".json";
    fs::path p = get_lynx_cache_dir() / "tmp" / oss.str();
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    return p.string();
}

void PackageInstaller::safe_remove(const fs::path& p) {
    std::error_code ec;
    if (fs::exists(p, ec)) fs::remove(p, ec);
}

void PackageInstaller::clear_summary() {
    std::lock_guard<std::mutex> lock(install_mutex);
    skipped_packages.clear();
    installed_summary_packages.clear();
    pending_lifecycle_packages.clear();
}

bool PackageInstaller::install_single_package(const std::string& raw_input, bool is_global,
                                              const InstallContext& ctx) {
    std::string package_name = raw_input;
    std::string requested_version;

    size_t at = raw_input.find('@');
    if (at == 0) at = raw_input.find('@', 1);
    if (at != std::string::npos && at > 0) {
        package_name = raw_input.substr(0, at);
        requested_version = raw_input.substr(at + 1);
    }

    // Circular dependency
    if (std::find(ctx.chain.begin(), ctx.chain.end(), package_name) != ctx.chain.end()) {
        std::lock_guard<std::mutex> lock(install_mutex);
        std::cout << "[Lynx]: Circular dependency on \"" << package_name << "\" — skipping.\n";
        skipped_packages.push_back(package_name + " (circular)");
        return true;
    }

    fs::path target_base = is_global ? (get_global_dir() / "node_modules")
                                      : (fs::current_path() / "node_modules");
    fs::path target_path = target_base / package_name;
    std::error_code ec;

    // Claim target path (TOCTOU)
    std::string claim_key = target_path.string();
    {
        std::lock_guard<std::mutex> lock(install_mutex);
        if (in_progress_targets.count(claim_key)) return true;
        in_progress_targets.insert(claim_key);
    }
    ClaimGuard claim_guard(this, claim_key);

    // Resolve version + metadata
    std::string target_version, tarball_url, integrity;
    std::map<std::string, std::string> dep_map;

    LockPackage locked;
    bool have_locked_match = !is_global && g_lockfile.get_package_info(package_name, locked) &&
        (requested_version.empty() || requested_version == "*" || requested_version == "latest" ||
         version_satisfies_range(locked.version, requested_version));

    if (have_locked_match) {
        target_version = locked.version;
        tarball_url = locked.resolved;
        integrity = locked.integrity;
        dep_map = locked.dependencies;
    } else {
        std::string tmp = make_unique_temp(package_name);
        TempFileCleaner cleaner{tmp};
        std::string url = "https://registry.npmjs.org/" + package_name;

        std::string cmd = "curl -s -L -H " + shell_quote("Accept: application/vnd.npm.install-v1+json") +
                           " " + shell_quote(url) + " -o " + shell_quote(tmp);
        if (std::system(cmd.c_str()) != 0 || !fs::exists(tmp, ec) || fs::file_size(tmp, ec) == 0) {
            std::lock_guard<std::mutex> lock(install_mutex);
            std::cerr << "[Lynx ERROR]: Package " << package_name << " not found\n";
            return false;
        }

        try {
            json meta;
            std::ifstream f(tmp);
            f >> meta;
            safe_remove(tmp);

            target_version = resolve_best_version(meta, requested_version);
            if (target_version.empty() || !meta["versions"].contains(target_version)) {
                std::lock_guard<std::mutex> lock(install_mutex);
                std::cerr << "[Lynx ERROR]: Cannot resolve " << package_name << "\n";
                return false;
            }

            auto& vmeta = meta["versions"][target_version];
            tarball_url = vmeta["dist"]["tarball"].get<std::string>();
            if (vmeta["dist"].contains("integrity")) integrity = vmeta["dist"]["integrity"];
            else if (vmeta["dist"].contains("shasum")) integrity = vmeta["dist"]["shasum"];

            if (vmeta.contains("dependencies")) {
                for (auto& [k, v] : vmeta["dependencies"].items())
                    dep_map[k] = v.get<std::string>();
            }
            if (vmeta.contains("optionalDependencies")) {
                for (auto& [k, v] : vmeta["optionalDependencies"].items())
                    dep_map[k] = v.get<std::string>();
            }
        } catch (...) {
            return false;
        }
    }

    // Conflict / up-to-date check
    {
        std::lock_guard<std::mutex> lock(install_mutex);
        std::string installed_version = read_installed_version(target_path);
        if (!installed_version.empty()) {
            if (installed_version == target_version) {
                skipped_packages.push_back(package_name + "@" + target_version + " (up to date)");
                return true;
            }

            // Chỉ skip khi là transitive THUẦN (không được phép overwrite)
            if (!ctx.is_direct && !ctx.allow_overwrite) {
                std::cerr << "[Lynx WARNING]: " << package_name << "@" << installed_version
                          << " is already installed; skipping conflicting request for "
                          << package_name << "@" << target_version
                          << " (flat install — first resolved version wins)\n";
                skipped_packages.push_back(package_name + "@" + target_version +
                                            " (conflict, kept " + installed_version + ")");
                return true;
            }
            // is_direct hoặc allow_overwrite → fall through, upgrade
        }
    }

    // Dedup trong cùng phiên
    std::string resolved_key = target_path.string() + "@" + target_version;
    {
        std::lock_guard<std::mutex> lock(install_mutex);
        if (installed_packages.count(resolved_key)) return true;
        installed_packages.insert(resolved_key);
    }

    // Download + CAS
    bool need_download = !is_package_in_cas(package_name, target_version);
    if (need_download) {
        fs::path cache_dir = get_lynx_cache_dir();
        std::string archive = sanitize_filename(package_name) + "-" + target_version + ".tgz";
        fs::path tgz = cache_dir / archive;
        fs::path extract_dir = cache_dir / "extracted" / (sanitize_filename(package_name) + "_" + target_version);

        if (!fs::exists(tgz, ec) || fs::file_size(tgz, ec) == 0) {
            {
                std::lock_guard<std::mutex> lock(install_mutex);
                std::cout << "[Lynx]: Fetching " << package_name << "@" << target_version << "...\n" << std::flush;
            }
            fs::path tmp_tgz = cache_dir / (archive + ".part." + std::to_string(g_temp_counter.fetch_add(1)));
            std::string dl = "curl -s -L " + shell_quote(tarball_url) + " -o " + shell_quote(tmp_tgz.string());
            if (std::system(dl.c_str()) != 0 || !fs::exists(tmp_tgz, ec)) {
                safe_remove(tmp_tgz);
                return false;
            }
            fs::rename(tmp_tgz, tgz, ec);
        }

        {
            std::lock_guard<std::mutex> lock(install_mutex);
            if (fs::exists(extract_dir, ec)) fs::remove_all(extract_dir, ec);
            fs::create_directories(extract_dir, ec);

            std::string tar = "tar -xzf " + shell_quote(tgz.string()) + " -C " + shell_quote(extract_dir.string()) +
                               " --strip-components=1";
            if (std::system(tar.c_str()) != 0) {
                fs::remove_all(extract_dir, ec);
                return false;
            }
            if (!import_package_to_cas(extract_dir, package_name, target_version)) {
                fs::remove_all(extract_dir, ec);
                return false;
            }
            fs::remove_all(extract_dir, ec);
        }
    }

    // Materialize
    {
        std::lock_guard<std::mutex> lock(install_mutex);
        if (fs::exists(target_path, ec)) fs::remove_all(target_path, ec);

        if (!materialize_from_cas(package_name, target_version, target_path)) {
            std::cerr << "[Lynx ERROR]: Materialize failed: " << package_name << "\n";
            return false;
        }

        if (is_global) {
            generate_bin_shims(target_path, package_name, get_global_bin_dir());
        } else if (ctx.is_direct) {
            generate_bin_shims(target_path, package_name);
        }

        std::cout << "[Lynx]: Done! " << package_name << "@" << target_version
                  << (need_download ? " downloaded" : " from CAS") << "\n" << std::flush;

        installed_summary_packages.push_back(package_name + "@" + target_version);
        pending_lifecycle_packages.emplace_back(target_path, package_name);
    }

    if (!is_global && ctx.is_direct) {
        g_lockfile.add_package(package_name, target_version, tarball_url, integrity, dep_map);
    }

    // Dependency con — kế thừa quyền overwrite từ cha
    if (!dep_map.empty()) {
        InstallContext child_ctx;
        child_ctx.is_direct = false;
        child_ctx.allow_overwrite = ctx.is_direct || ctx.allow_overwrite;
        child_ctx.chain = ctx.chain;
        child_ctx.chain.push_back(package_name);

        std::vector<std::string> child_deps;
        for (const auto& [n, v] : dep_map) {
            child_deps.push_back(n + "@" + v);
        }
        install_packages_parallel(child_deps, is_global, child_ctx);
    }

    return true;
}

void PackageInstaller::install_packages_parallel(const std::vector<std::string>& targets, bool is_global,
                                                 const InstallContext& ctx) {
    if (targets.empty()) return;

    std::vector<std::future<bool>> jobs;
    jobs.reserve(targets.size());

    for (const auto& t : targets) {
        while ((int)jobs.size() >= LYNX_MAX_PARALLEL) {
            bool progressed = false;
            for (auto it = jobs.begin(); it != jobs.end(); ) {
                if (it->wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                    try { it->get(); } catch (...) {}
                    it = jobs.erase(it);
                    progressed = true;
                } else ++it;
            }
            if (!progressed) std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        jobs.emplace_back(std::async(std::launch::async,
            &PackageInstaller::install_single_package, this, t, is_global, ctx));
    }

    for (auto& j : jobs) {
        try { j.get(); } catch (...) {}
    }
}

void PackageInstaller::run_all_pending_lifecycles() {
    if (pending_lifecycle_packages.empty()) return;
    std::cout << "\n[Lynx]: Running lifecycle scripts...\n";
    for (auto& [path, name] : pending_lifecycle_packages) {
        run_lifecycle_scripts(path, name, false);
    }
    pending_lifecycle_packages.clear();
}

void PackageInstaller::print_summary() {
    std::cout << "\n--------------------------------------------------\n[Lynx Summary]:\n";
    if (!installed_summary_packages.empty()) {
        std::cout << "  Installed (" << installed_summary_packages.size() << "):\n";
        for (auto& p : installed_summary_packages) std::cout << "    + " << p << "\n";
    }
    if (!skipped_packages.empty()) {
        std::cout << "  Skipped (" << skipped_packages.size() << "):\n";
        for (auto& p : skipped_packages) std::cout << "    - " << p << "\n";
    }
    if (installed_summary_packages.empty() && skipped_packages.empty()) {
        std::cout << "  Nothing to do.\n";
    }
    std::cout << "--------------------------------------------------\n";
}