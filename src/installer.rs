use crate::lockfile::lockfile;
use crate::utils::*;
use futures::stream::{FuturesUnordered, StreamExt};
use parking_lot::Mutex;
use serde_json::Value;
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use tokio::sync::Semaphore;
use anyhow::{Context, Result};

#[derive(Clone, Default)]
pub struct InstallContext {
    pub is_direct: bool,
    pub allow_overwrite: bool,
    pub chain: Vec<String>,
}

#[derive(Clone, Default)]
struct SemVer {
    major: i32,
    minor: i32,
    patch: i32,
    is_prerelease: bool,
    prerelease_tag: String,
}

impl SemVer {
    fn parse(v: &str) -> Self {
        let mut clean = v.trim().to_string();
        while let Some(c) = clean.chars().next() {
            if "^~=><".contains(c) {
                clean.remove(0);
            } else {
                break;
            }
        }
        let (main, pre) = match clean.split_once('-') {
            Some((a, b)) => (a.to_string(), Some(b.to_string())),
            None => (clean, None),
        };
        let mut parts = main.split('.');
        let major = parts.next().and_then(|s| s.parse().ok()).unwrap_or(0);
        let minor = parts.next().and_then(|s| s.parse().ok()).unwrap_or(0);
        let patch = parts.next().and_then(|s| s.parse().ok()).unwrap_or(0);
        Self {
            major,
            minor,
            patch,
            is_prerelease: pre.is_some(),
            prerelease_tag: pre.unwrap_or_default(),
        }
    }

    fn cmp_key(&self) -> (i32, i32, i32, bool, &str) {
        // release > prerelease for same numbers: !is_prerelease sorts higher if we invert later
        (
            self.major,
            self.minor,
            self.patch,
            !self.is_prerelease,
            self.prerelease_tag.as_str(),
        )
    }
}

fn version_gt(a: &SemVer, b: &SemVer) -> bool {
    a.cmp_key() > b.cmp_key()
}

fn version_ge(a: &SemVer, b: &SemVer) -> bool {
    a.cmp_key() >= b.cmp_key()
}

fn version_eq(a: &SemVer, b: &SemVer) -> bool {
    a.major == b.major
        && a.minor == b.minor
        && a.patch == b.patch
        && a.is_prerelease == b.is_prerelease
        && a.prerelease_tag == b.prerelease_tag
}

fn resolve_best_version(meta: &Value, range_req: &str) -> Option<String> {
    let versions = meta.get("versions")?.as_object()?;
    let mut range = range_req.trim().to_string();
    range.retain(|c| !c.is_whitespace());

    if range.is_empty() || range == "*" || range == "latest" || range == "x" || range == "X" {
        if let Some(latest) = meta.pointer("/dist-tags/latest").and_then(|v| v.as_str()) {
            return Some(latest.to_string());
        }
    }
    if versions.contains_key(&range) {
        return Some(range);
    }

    let mut op = "^".to_string();
    let mut clean = range.clone();
    if let Some(rest) = clean.strip_prefix(">=") {
        op = ">=".into();
        clean = rest.to_string();
    } else if let Some(rest) = clean.strip_prefix("<=") {
        op = "<=".into();
        clean = rest.to_string();
    } else if let Some(rest) = clean.strip_prefix('>') {
        op = ">".into();
        clean = rest.to_string();
    } else if let Some(rest) = clean.strip_prefix('<') {
        op = "<".into();
        clean = rest.to_string();
    } else if let Some(c) = clean.chars().next() {
        if "^~=".contains(c) {
            op = c.to_string();
            clean = clean[1..].to_string();
        }
    }

    let target = SemVer::parse(&clean);
    let want_pre = target.is_prerelease || range.contains('-');

    let mut best: Option<(String, SemVer)> = None;
    for (ver_str, _) in versions {
        let curr = SemVer::parse(ver_str);
        if curr.is_prerelease && !want_pre {
            continue;
        }
        let ok = match op.as_str() {
            "^" => curr.major == target.major && version_ge(&curr, &target),
            "~" => {
                curr.major == target.major
                    && curr.minor == target.minor
                    && curr.patch >= target.patch
            }
            "=" => version_eq(&curr, &target),
            ">=" => version_ge(&curr, &target),
            ">" => version_gt(&curr, &target),
            "<=" => !version_gt(&curr, &target),
            "<" => version_gt(&target, &curr),
            _ => false,
        };
        if ok {
            if best
                .as_ref()
                .map(|(_, b)| version_gt(&curr, b))
                .unwrap_or(true)
            {
                best = Some((ver_str.clone(), curr));
            }
        }
    }
    if let Some((v, _)) = best {
        return Some(v);
    }

    // fallback: highest non-pre (or any if want_pre)
    let mut fallback: Option<(String, SemVer)> = None;
    for (ver_str, _) in versions {
        let curr = SemVer::parse(ver_str);
        if curr.is_prerelease && !want_pre {
            continue;
        }
        if fallback
            .as_ref()
            .map(|(_, b)| version_gt(&curr, b))
            .unwrap_or(true)
        {
            fallback = Some((ver_str.clone(), curr));
        }
    }
    fallback.map(|(v, _)| v)
}

fn version_satisfies_range(version_str: &str, range_req: &str) -> bool {
    let mut range = range_req.trim().to_string();
    range.retain(|c| !c.is_whitespace());
    if range.is_empty() || range == "*" || range == "latest" || range == "x" || range == "X" {
        return true;
    }
    if range == version_str {
        return true;
    }
    // reuse resolve-style match on single version via synthetic meta is heavy;
    // inline same ops:
    let mut op = "^".to_string();
    let mut clean = range.clone();
    if let Some(rest) = clean.strip_prefix(">=") {
        op = ">=".into();
        clean = rest.to_string();
    } else if let Some(rest) = clean.strip_prefix("<=") {
        op = "<=".into();
        clean = rest.to_string();
    } else if let Some(rest) = clean.strip_prefix('>') {
        op = ">".into();
        clean = rest.to_string();
    } else if let Some(rest) = clean.strip_prefix('<') {
        op = "<".into();
        clean = rest.to_string();
    } else if let Some(c) = clean.chars().next() {
        if "^~=".contains(c) {
            op = c.to_string();
            clean = clean[1..].to_string();
        }
    }
    let target = SemVer::parse(&clean);
    let curr = SemVer::parse(version_str);
    let want_pre = target.is_prerelease || range.contains('-');
    if curr.is_prerelease && !want_pre {
        return false;
    }
    match op.as_str() {
        "^" => curr.major == target.major && version_ge(&curr, &target),
        "~" => {
            curr.major == target.major && curr.minor == target.minor && curr.patch >= target.patch
        }
        "=" => version_eq(&curr, &target),
        ">=" => version_ge(&curr, &target),
        ">" => version_gt(&curr, &target),
        "<=" => !version_gt(&curr, &target),
        "<" => version_gt(&target, &curr),
        _ => false,
    }
}

pub struct PackageInstaller {
    skipped: Mutex<Vec<String>>,
    installed_summary: Mutex<Vec<String>>,
    in_progress: Mutex<HashSet<String>>,
    session_done: Mutex<HashSet<String>>,
}

impl PackageInstaller {
    pub fn new() -> Self {
        Self {
            skipped: Mutex::new(Vec::new()),
            installed_summary: Mutex::new(Vec::new()),
            in_progress: Mutex::new(HashSet::new()),
            session_done: Mutex::new(HashSet::new()),
        }
    }

    pub fn clear_summary(&self) {
        self.skipped.lock().clear();
        self.installed_summary.lock().clear();
        self.session_done.lock().clear();
        self.in_progress.lock().clear();
    }

    pub fn print_summary(&self) {
        println!("\n--------------------------------------------------");
        println!("[Lynx Summary]:");
        let inst = self.installed_summary.lock();
        if !inst.is_empty() {
            println!("  Installed ({}):", inst.len());
            for p in inst.iter() {
                println!("    + {p}");
            }
        }
        let skip = self.skipped.lock();
        if !skip.is_empty() {
            println!("  Skipped ({}):", skip.len());
            for p in skip.iter() {
                println!("    - {p}");
            }
        }
        if inst.is_empty() && skip.is_empty() {
            println!("  Nothing to do.");
        }
        println!("--------------------------------------------------");
    }

    pub async fn install_single(
        self: &Arc<Self>,
        raw: &str,
        is_global: bool,
        ctx: InstallContext,
    ) -> Result<()> {
        let (package_name, requested_version) = parse_raw(raw);

        // Circular dependency
        if ctx.chain.iter().any(|n| n == &package_name) {
            self.skipped
                .lock()
                .push(format!("{package_name} (circular)"));
            return Ok(());
        }

        let target_base = if is_global {
            global_dir().join("node_modules")
        } else {
            PathBuf::from("node_modules")
        };
        let target_path = target_base.join(&package_name);

        // Claim path (TOCTOU)
        let claim_key = target_path.to_string_lossy().to_string();
        {
            let mut ip = self.in_progress.lock();
            if ip.contains(&claim_key) {
                return Ok(());
            }
            ip.insert(claim_key.clone());
        }
        struct Guard<'a>(&'a Mutex<HashSet<String>>, String);
        impl Drop for Guard<'_> {
            fn drop(&mut self) {
                self.0.lock().remove(&self.1);
            }
        }
        let _guard = Guard(&self.in_progress, claim_key);

        // Resolve version + metadata
        let locked = if !is_global {
            lockfile().get(&package_name)
        } else {
            None
        };

        let use_lock = locked.as_ref().map_or(false, |lp| {
            requested_version.is_empty()
                || requested_version == "*"
                || requested_version == "latest"
                || version_satisfies_range(&lp.version, &requested_version)
        });

        let (target_version, tarball_url, integrity, dep_map) = if use_lock {
            let lp = locked.unwrap();
            (
                lp.version,
                lp.resolved,
                lp.integrity,
                lp.dependencies,
            )
        } else {
            let url = format!("https://registry.npmjs.org/{package_name}");
            let client = reqwest::Client::new();
            let meta: Value = client
                .get(&url)
                .header("Accept", "application/vnd.npm.install-v1+json")
                .send()
                .await?
                .error_for_status()?
                .json()
                .await
                .context("registry json")?;

            let target_version = resolve_best_version(&meta, &requested_version)
                .ok_or_else(|| anyhow::anyhow!("Cannot resolve {package_name}"))?;
            let vmeta = meta
                .pointer(&format!("/versions/{target_version}"))
                .ok_or_else(|| anyhow::anyhow!("version missing in registry"))?;
            let tarball_url = vmeta["dist"]["tarball"]
                .as_str()
                .unwrap_or("")
                .to_string();
            let integrity = vmeta["dist"]
                .get("integrity")
                .or_else(|| vmeta["dist"].get("shasum"))
                .and_then(|v| v.as_str())
                .unwrap_or("")
                .to_string();

            let mut dep_map: HashMap<String, String> = HashMap::new();
            if let Some(deps) = vmeta.get("dependencies").and_then(|d| d.as_object()) {
                for (k, v) in deps {
                    if let Some(s) = v.as_str() {
                        dep_map.insert(k.clone(), s.to_string());
                    }
                }
            }
            if let Some(deps) = vmeta
                .get("optionalDependencies")
                .and_then(|d| d.as_object())
            {
                for (k, v) in deps {
                    if let Some(s) = v.as_str() {
                        dep_map.insert(k.clone(), s.to_string());
                    }
                }
            }
            (target_version, tarball_url, integrity, dep_map)
        };

        // Conflict / up-to-date
        if let Some(installed) = read_installed_version(&target_path) {
            if installed == target_version {
                self.skipped
                    .lock()
                    .push(format!("{package_name}@{target_version} (up to date)"));
                return Ok(());
            }
            if !ctx.is_direct && !ctx.allow_overwrite {
                eprintln!(
                    "[Lynx WARNING]: {package_name}@{installed} already installed; \
                     skipping {package_name}@{target_version}"
                );
                self.skipped.lock().push(format!(
                    "{package_name}@{target_version} (conflict, kept {installed})"
                ));
                return Ok(());
            }
        }

        // Dedup trong cùng phiên
        let resolved_key = format!("{}@{target_version}", target_path.display());
        {
            let mut done = self.session_done.lock();
            if done.contains(&resolved_key) {
                return Ok(());
            }
            done.insert(resolved_key);
        }

        // Download + CAS
        let need_download = !is_package_in_cas(&package_name, &target_version);
        if need_download {
            let cache = lynx_cache_dir();
            let archive = format!(
                "{}-{}.tgz",
                sanitize_filename(&package_name),
                target_version
            );
            let tgz = cache.join(&archive);

            if !tgz.exists() || tgz.metadata().map(|m| m.len()).unwrap_or(0) == 0 {
                println!("[Lynx]: Fetching {package_name}@{target_version}...");
                download_file(&tarball_url, &tgz).await?;
            }

            let extract_dir = cache.join("extracted").join(format!(
                "{}_{}",
                sanitize_filename(&package_name),
                target_version
            ));
            if extract_dir.exists() {
                fs_remove(&extract_dir)?;
            }
            extract_tgz(&tgz, &extract_dir)?;
            import_package_to_cas(&extract_dir, &package_name, &target_version)?;
            let _ = fs_remove(&extract_dir);
        }

        // Materialize
        if target_path.exists() {
            fs_remove(&target_path)?;
        }
        materialize_from_cas(&package_name, &target_version, &target_path)?;

        if is_global {
            generate_bin_shims(&target_path, &package_name, Some(global_bin_dir()));
        } else if ctx.is_direct {
            generate_bin_shims(&target_path, &package_name, None);
        }

        println!(
            "[Lynx]: Done! {package_name}@{target_version}{}",
            if need_download {
                " downloaded"
            } else {
                " from CAS"
            }
        );
        self.installed_summary
            .lock()
            .push(format!("{package_name}@{target_version}"));

        if !is_global && ctx.is_direct {
            lockfile().add(
                &package_name,
                &target_version,
                &tarball_url,
                &integrity,
                dep_map.clone(),
            );
        }

        // Dependency con — kế thừa allow_overwrite
        if !dep_map.is_empty() {
            let child_ctx = InstallContext {
                is_direct: false,
                allow_overwrite: ctx.is_direct || ctx.allow_overwrite,
                chain: {
                    let mut c = ctx.chain.clone();
                    c.push(package_name.clone());
                    c
                },
            };
            let child_deps: Vec<String> = dep_map
                .into_iter()
                .map(|(n, v)| format!("{n}@{v}"))
                .collect();
            self.install_parallel(child_deps, is_global, child_ctx)
                .await?;
        }

        Ok(())
    }

    pub async fn install_parallel(
        self: &Arc<Self>,
        targets: Vec<String>,
        is_global: bool,
        ctx: InstallContext,
    ) -> Result<()> {
        let sem = Arc::new(Semaphore::new(MAX_PARALLEL));
        let mut futs = FuturesUnordered::new();
        for t in targets {
            let permit = sem.clone().acquire_owned().await?;
            let this = Arc::clone(self);
            let ctx = ctx.clone();
            futs.push(async move {
                let _p = permit;
                this.install_single(&t, is_global, ctx).await
            });
        }
        while let Some(r) = futs.next().await {
            if let Err(e) = r {
                eprintln!("[Lynx ERROR]: {e:#}");
            }
        }
        Ok(())
    }
}

fn parse_raw(raw: &str) -> (String, String) {
    // scoped: @scope/name@version
    if let Some(rest) = raw.strip_prefix('@') {
        if let Some(idx) = rest.find('@') {
            let name = format!("@{}", &rest[..idx]);
            let ver = rest[idx + 1..].to_string();
            return (name, ver);
        }
        return (raw.to_string(), String::new());
    }
    if let Some((name, ver)) = raw.split_once('@') {
        return (name.to_string(), ver.to_string());
    }
    (raw.to_string(), String::new())
}

fn fs_remove(p: &Path) -> Result<()> {
    if p.is_dir() {
        std::fs::remove_dir_all(p)?;
    } else if p.exists() {
        std::fs::remove_file(p)?;
    }
    Ok(())
}