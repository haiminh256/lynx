use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

pub const MAX_PARALLEL: usize = 30;

pub fn lynx_cache_dir() -> PathBuf {
    if let Ok(xdg) = std::env::var("XDG_CACHE_HOME") {
        return PathBuf::from(xdg).join("lynx");
    }
    if let Some(home) = dirs::home_dir() {
        #[cfg(windows)]
        {
            return home.join(".lynx");
        }
        #[cfg(not(windows))]
        {
            return home.join(".cache").join("lynx");
        }
    }
    PathBuf::from(".lynx_cache")
}

pub fn global_dir() -> PathBuf {
    #[cfg(windows)]
    {
        if let Ok(appdata) = std::env::var("APPDATA") {
            let p = PathBuf::from(appdata).join("lynx");
            let _ = fs::create_dir_all(p.join("node_modules"));
            return p;
        }
    }
    let p = dirs::home_dir()
        .unwrap_or_else(|| PathBuf::from("."))
        .join(".lynx")
        .join("global");
    let _ = fs::create_dir_all(p.join("node_modules"));
    p
}

pub fn global_bin_dir() -> PathBuf {
    #[cfg(windows)]
    {
        return global_dir();
    }
    #[cfg(not(windows))]
    {
        let p = global_dir().join("bin");
        let _ = fs::create_dir_all(&p);
        p
    }
}

pub fn sanitize_filename(name: &str) -> String {
    name.chars()
        .map(|c| match c {
            '/' | '\\' | '@' | ':' | '*' => '_',
            _ => c,
        })
        .collect()
}

pub fn sha256_file(path: &Path) -> Result<String> {
    let data = fs::read(path)?;
    let mut hasher = Sha256::new();
    hasher.update(&data);
    Ok(hex::encode(hasher.finalize()))
}

pub fn add_to_cas(source: &Path) -> Result<PathBuf> {
    let hash = sha256_file(source)?;
    let store = lynx_cache_dir()
        .join("store")
        .join(&hash[..2])
        .join(&hash);
    if !store.exists() {
        fs::create_dir_all(store.parent().unwrap())?;
        fs::copy(source, &store)?;
    }
    Ok(store)
}

#[derive(Serialize, Deserialize)]
pub struct CasFile {
    pub path: String,
    pub hash: String,
}

#[derive(Serialize, Deserialize)]
struct CasIndex {
    name: String,
    version: String,
    files: Vec<CasFile>,
}

pub fn import_package_to_cas(extracted: &Path, name: &str, version: &str) -> Result<()> {
    let mut files = Vec::new();
    for entry in walkdir::WalkDir::new(extracted)
        .into_iter()
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().is_file())
    {
        let rel = entry
            .path()
            .strip_prefix(extracted)?
            .to_string_lossy()
            .replace('\\', "/");
        let store = add_to_cas(entry.path())?;
        files.push(CasFile {
            path: rel,
            hash: store.file_name().unwrap().to_string_lossy().into(),
        });
    }
    let index_dir = lynx_cache_dir().join("index");
    fs::create_dir_all(&index_dir)?;
    let index_path = index_dir.join(format!("{}@{}.json", sanitize_filename(name), version));
    let idx = CasIndex {
        name: name.to_string(),
        version: version.to_string(),
        files,
    };
    fs::write(index_path, serde_json::to_string_pretty(&idx)?)?;
    Ok(())
}

pub fn is_package_in_cas(name: &str, version: &str) -> bool {
    lynx_cache_dir()
        .join("index")
        .join(format!("{}@{}.json", sanitize_filename(name), version))
        .exists()
}

pub fn materialize_from_cas(name: &str, version: &str, target: &Path) -> Result<()> {
    let index_path = lynx_cache_dir()
        .join("index")
        .join(format!("{}@{}.json", sanitize_filename(name), version));
    let idx: CasIndex =
        serde_json::from_str(&fs::read_to_string(&index_path).context("CAS index")?)?;
    if target.exists() {
        fs::remove_dir_all(target)?;
    }
    fs::create_dir_all(target)?;
    for f in idx.files {
        let store = lynx_cache_dir()
            .join("store")
            .join(&f.hash[..2])
            .join(&f.hash);
        let dest = target.join(&f.path);
        if let Some(parent) = dest.parent() {
            fs::create_dir_all(parent)?;
        }
        if fs::hard_link(&store, &dest).is_err() {
            fs::copy(&store, &dest)?;
        }
    }
    Ok(())
}

pub fn read_installed_version(package_dir: &Path) -> Option<String> {
    let pj = package_dir.join("package.json");
    let data = fs::read_to_string(pj).ok()?;
    let j: serde_json::Value = serde_json::from_str(&data).ok()?;
    j.get("version")?.as_str().map(|s| s.to_string())
}

pub fn generate_bin_shims(package_path: &Path, package_name: &str, bin_dir: Option<PathBuf>) {
    let pj = package_path.join("package.json");
    let Ok(data) = fs::read_to_string(&pj) else {
        return;
    };
    let Ok(j) = serde_json::from_str::<serde_json::Value>(&data) else {
        return;
    };
    let Some(bin) = j.get("bin") else {
        return;
    };
    let dir = bin_dir.unwrap_or_else(|| PathBuf::from("node_modules/.bin"));
    let _ = fs::create_dir_all(&dir);

    let mut entries: Vec<(String, String)> = Vec::new();
    if let Some(s) = bin.as_str() {
        let name = package_name.rsplit('/').next().unwrap_or(package_name);
        entries.push((name.to_string(), s.to_string()));
    } else if let Some(obj) = bin.as_object() {
        for (k, v) in obj {
            if let Some(s) = v.as_str() {
                entries.push((k.clone(), s.to_string()));
            }
        }
    }

    for (bin_name, rel) in entries {
        #[cfg(windows)]
        {
            let cmd_path = dir.join(format!("{bin_name}.cmd"));
            let target = package_path.join(&rel);
            let body = format!(
                "@SETLOCAL\n@IF EXIST \"%~dp0\\node.exe\" (\n  \"%~dp0\\node.exe\" \"{}\" %*\n) ELSE (\n  node \"{}\" %*\n)\n",
                target.display(),
                target.display()
            );
            let _ = fs::write(cmd_path, body);
        }
        #[cfg(not(windows))]
        {
            let sh_path = dir.join(&bin_name);
            let target = package_path.join(&rel);
            let body = format!(
                "#!/bin/sh\nexec node \"{}\" \"$@\"\n",
                target.display()
            );
            let _ = fs::write(&sh_path, body);
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                if let Ok(meta) = fs::metadata(&sh_path) {
                    let mut p = meta.permissions();
                    p.set_mode(0o755);
                    let _ = fs::set_permissions(&sh_path, p);
                }
            }
        }
    }
}

pub fn run_cmd(cmd: &str, cwd: Option<&Path>, extra_path: Option<&Path>) -> Result<i32> {
    let old = std::env::var("PATH").unwrap_or_default();
    let path = if let Some(bin) = extra_path {
        #[cfg(windows)]
        {
            format!("{};{}", bin.display(), old)
        }
        #[cfg(not(windows))]
        {
            format!("{}:{}", bin.display(), old)
        }
    } else {
        old
    };

    #[cfg(windows)]
    {
        let mut c = Command::new("cmd");
        c.args(["/d", "/s", "/c", cmd]);
        c.env("PATH", &path);
        if let Some(d) = cwd {
            c.current_dir(d);
        }
        Ok(c.status()?.code().unwrap_or(1))
    }
    #[cfg(not(windows))]
    {
        let mut c = Command::new("sh");
        c.args(["-c", cmd]);
        c.env("PATH", &path);
        if let Some(d) = cwd {
            c.current_dir(d);
        }
        Ok(c.status()?.code().unwrap_or(1))
    }
}

pub async fn download_file(url: &str, dest: &Path) -> Result<()> {
    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent)?;
    }
    let bytes = reqwest::get(url).await?.error_for_status()?.bytes().await?;
    fs::write(dest, &bytes)?;
    Ok(())
}

pub fn extract_tgz(tgz: &Path, out_dir: &Path) -> Result<()> {
    fs::create_dir_all(out_dir)?;
    let file = fs::File::open(tgz)?;
    let dec = flate2::read::GzDecoder::new(file);
    let mut archive = tar::Archive::new(dec);
    // npm packs have a top-level "package/" folder
    archive.unpack(out_dir)?;
    // strip package/ if present
    let nested = out_dir.join("package");
    if nested.is_dir() {
        for entry in fs::read_dir(&nested)? {
            let entry = entry?;
            let to = out_dir.join(entry.file_name());
            if to.exists() {
                if to.is_dir() {
                    fs::remove_dir_all(&to)?;
                } else {
                    fs::remove_file(&to)?;
                }
            }
            fs::rename(entry.path(), to)?;
        }
        let _ = fs::remove_dir_all(nested);
    }
    Ok(())
}