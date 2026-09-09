mod app;
mod commands;
mod installer;
mod lockfile;
mod utils;

use app::Cli;
use clap::Parser;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let cli = Cli::parse();
    cli.run().await
}