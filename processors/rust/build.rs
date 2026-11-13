use std::{env, process::Command};
fn main() {
    let output = Command::new(env::var_os("RUSTC").unwrap())
        .arg("--version")
        .output()
        .unwrap();
    println!(
        "cargo:rustc-env=PROCESSOR_COMPILER={}",
        String::from_utf8(output.stdout).unwrap().trim()
    );
    println!(
        "cargo:rustc-env=PROCESSOR_BUILD_TYPE={}",
        env::var("PROFILE").unwrap()
    );
}
