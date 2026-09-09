use crate::installer::{InstallContext, PackageInstaller};
use crate::lockfile::lockfile;
use crate::utils::{global_dir, run_cmd};
use anyhow::{bail, Result};
use std::fs;
use std::path::Path;
use std::sync::Arc;

pub async fn install(packages: Vec<String>, global: bool, save_dev: bool) -> Result<()> {
    let installer = Arc::new(PackageInstaller::new());
    installer.clear_summary();

    let ctx = InstallContext {
        is_direct: true,
        allow_overwrite: true,
        chain: vec![],
    };

    if packages.is_empty() {
        if global {
            bail!("Please specify packages to install globally");
        }
        if !Path::new("package.json").exists() {
            bail!("No package.json found");
        }
        let pkg: serde_json::Value = serde_json::from_str(&fs::read_to_string("package.json")?)?;
        let mut all = Vec::new();
        for key in [
            "dependencies",
            "devDependencies",
            "peerDependencies",
            "optionalDependencies",
        ] {
            if let Some(obj) = pkg.get(key).and_then(|v| v.as_object()) {
                for (name, ver) in obj {
                    if let Some(v) = ver.as_str() {
                        all.push(format!("{name}@{v}"));
                    }
                }
            }
        }
        if all.is_empty() {
            println!("[Lynx]: No dependencies found.");
        } else {
            println!("[Lynx]: Installing {} packages...\n", all.len());
            installer.install_parallel(all, false, ctx).await?;
            installer.print_summary();
        }
    } else {
        installer
            .install_parallel(packages.clone(), global, ctx)
            .await?;
        installer.print_summary();

        if !global && Path::new("package.json").exists() {
            let mut pkg: serde_json::Value =
                serde_json::from_str(&fs::read_to_string("package.json")?)?;
            let section = if save_dev {
                "devDependencies"
            } else {
                "dependencies"
            };
            if pkg.get(section).is_none() {
                pkg[section] = serde_json::json!({});
            }
            for raw in &packages {
                let (name, ver) = if raw.starts_with('@') {
                    if let Some(i) = raw[1..].find('@') {
                        (
                            format!("@{}", &raw[1..1 + i]),
                            raw[2 + i..].to_string(),
                        )
                    } else {
                        (raw.clone(), String::new())
                    }
                } else if let Some((n, v)) = raw.split_once('@') {
                    (n.to_string(), v.to_string())
                } else {
                    (raw.clone(), String::new())
                };
                let ver = if ver.is_empty() {
                    lockfile()
                        .get(&name)
                        .map(|p| format!("^{}", p.version))
                        .unwrap_or_else(|| "*".into())
                } else if ver.starts_with('^') || ver.starts_with('~') {
                    ver
                } else {
                    format!("^{ver}")
                };
                pkg[section][&name] = serde_json::Value::String(ver);
            }
            fs::write("package.json", serde_json::to_string_pretty(&pkg)?)?;
            println!("[Lynx]: Saved to {section} in package.json");
        }
    }

    if !global {
        lockfile().save()?;
        println!("[Lynx]: Updated lynx-lock.json");
    }
    Ok(())
}

pub async fn uninstall(package: &str, global: bool) -> Result<()> {
    let base = if global {
        global_dir().join("node_modules")
    } else {
        Path::new("node_modules").to_path_buf()
    };
    let target = base.join(package);
    if target.exists() {
        fs::remove_dir_all(&target)?;
        if !global {
            lockfile().remove(package);
            lockfile().save()?;
        }
        println!("[Lynx]: Successfully uninstalled {package}");
    } else {
        bail!("Package {package} is not installed");
    }
    Ok(())
}

pub async fn run(script: Option<String>, extra: Vec<String>) -> Result<()> {
    let pkg: serde_json::Value = serde_json::from_str(&fs::read_to_string("package.json")?)?;
    let Some(name) = script else {
        if let Some(scripts) = pkg.get("scripts").and_then(|s| s.as_object()) {
            println!("[Lynx]: Available scripts:\n");
            for (k, v) in scripts {
                println!("  {k}\n    {}\n", v.as_str().unwrap_or(""));
            }
        }
        return Ok(());
    };
    let cmd = pkg["scripts"][&name]
        .as_str()
        .ok_or_else(|| anyhow::anyhow!("Script \"{name}\" not found"))?;
    let mut full = cmd.to_string();
    for a in extra {
        full.push(' ');
        full.push_str(&a);
    }
    let bin = std::env::current_dir()?.join("node_modules").join(".bin");
    let code = run_cmd(&full, Some(Path::new(".")), Some(&bin))?;
    if code != 0 {
        bail!("script exited with {code}");
    }
    Ok(())
}

pub async fn create(template: &str, args: Vec<String>) -> Result<()> {
    // parse template: vue | vue@latest | create-vue@latest | @scope/create-x
    let mut name = template.to_string();

    // nếu chưa có prefix create- (trừ scoped package)
    if !name.starts_with("create-") && !name.starts_with('@') {
        name = format!("create-{name}");
    } else if name.starts_with('@') {
        // @scope/name@version — giữ nguyên
    } else if !name.starts_with("create-") {
        // name@version nhưng chưa có create-
        if let Some((n, ver)) = name.split_once('@') {
            if !n.starts_with("create-") {
                name = format!("create-{n}@{ver}");
            }
        }
    }

    let installer = Arc::new(PackageInstaller::new());
    installer
        .install_single(
            &name,
            false,
            InstallContext {
                is_direct: true,
                allow_overwrite: true,
                chain: vec![],
            },
        )
        .await?;
    installer.print_summary();

    // bin name: create-vue@latest -> create-vue
    let pkg_part = name.split('@').next().unwrap_or(&name);
    let bin = pkg_part.rsplit('/').next().unwrap_or(pkg_part);

    let cmd = if args.is_empty() {
        bin.to_string()
    } else {
        format!("{bin} {}", args.join(" "))
    };

    let bin_dir = Path::new("node_modules").join(".bin");
    let code = run_cmd(&cmd, Some(Path::new(".")), Some(&bin_dir))?;
    if code != 0 {
        bail!("create command exited with {code}");
    }
    Ok(())
}