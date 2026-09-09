use anyhow::Result;
use parking_lot::Mutex;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fs;
use std::path::Path;
use std::sync::OnceLock;

#[derive(Clone, Default, Serialize, Deserialize)]
pub struct LockPackage {
    pub version: String,
    pub resolved: String,
    #[serde(default)]
    pub integrity: String,
    #[serde(default)]
    pub dependencies: HashMap<String, String>,
}

#[derive(Serialize, Deserialize)]
struct LockFile {
    name: String,
    #[serde(rename = "lockfileVersion")]
    lockfile_version: u32,
    packages: HashMap<String, LockPackage>,
}

pub struct LockfileManager {
    path: String,
    packages: Mutex<HashMap<String, LockPackage>>,
}

static GLOBAL: OnceLock<LockfileManager> = OnceLock::new();

pub fn lockfile() -> &'static LockfileManager {
    GLOBAL.get_or_init(|| LockfileManager::new("lynx-lock.json"))
}

impl LockfileManager {
    pub fn new(path: &str) -> Self {
        let m = Self {
            path: path.to_string(),
            packages: Mutex::new(HashMap::new()),
        };
        let _ = m.load();
        m
    }

    pub fn load(&self) -> Result<()> {
        if !Path::new(&self.path).exists() {
            return Ok(());
        }
        let data = fs::read_to_string(&self.path)?;
        let j: LockFile = serde_json::from_str(&data)?;
        *self.packages.lock() = j.packages;
        Ok(())
    }

    pub fn save(&self) -> Result<()> {
        let packages = self.packages.lock().clone();
        let j = LockFile {
            name: "lynx-lockfile".into(),
            lockfile_version: 1,
            packages,
        };
        fs::write(&self.path, serde_json::to_string_pretty(&j)?)?;
        Ok(())
    }

    pub fn add(
        &self,
        name: &str,
        version: &str,
        resolved: &str,
        integrity: &str,
        deps: HashMap<String, String>,
    ) {
        self.packages.lock().insert(
            name.to_string(),
            LockPackage {
                version: version.into(),
                resolved: resolved.into(),
                integrity: integrity.into(),
                dependencies: deps,
            },
        );
    }

    pub fn remove(&self, name: &str) {
        self.packages.lock().remove(name);
    }

    pub fn get(&self, name: &str) -> Option<LockPackage> {
        self.packages.lock().get(name).cloned()
    }
}