use crate::commands;
use anyhow::Result;
use clap::{Parser, Subcommand};

#[derive(Parser, Debug)]
#[command(name = "lynx", about = "Fast Node package manager")]
pub struct Cli {
    #[command(subcommand)]
    pub command: Commands,
}

#[derive(Subcommand, Debug)]
pub enum Commands {
    Install {
        packages: Vec<String>,
        #[arg(short = 'g', long = "global")]
        global: bool,
        #[arg(short = 'D', long = "save-dev")]
        save_dev: bool,
    },
    Uninstall {
        package: String,
        #[arg(short = 'g', long = "global")]
        global: bool,
    },
    Run {
        script: Option<String>,
        #[arg(trailing_var_arg = true)]
        args: Vec<String>,
    },
    Create {
        template: String,
        #[arg(trailing_var_arg = true)]
        args: Vec<String>,
    },
}

impl Cli {
    pub async fn run(self) -> Result<()> {
        match self.command {
            Commands::Install {
                packages,
                global,
                save_dev,
            } => commands::install(packages, global, save_dev).await,
            Commands::Uninstall { package, global } => {
                commands::uninstall(&package, global).await
            }
            Commands::Run { script, args } => commands::run(script, args).await,
            Commands::Create { template, args } => commands::create(&template, args).await,
        }
    }
}