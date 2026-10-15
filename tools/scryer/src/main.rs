use clap::Parser;
use scryer_metrics::Cli;
use std::{
    io::{stderr, stdout},
    process::exit,
};

fn main() {
    let exit_code = scryer_metrics::run(Cli::parse(), stdout(), stderr());
    exit(exit_code);
}
