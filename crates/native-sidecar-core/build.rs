use base64::{engine::general_purpose::STANDARD, Engine as _};
use std::{env, fmt::Write as _, fs, path::PathBuf};
use webpki_root_certs::TLS_SERVER_ROOT_CERTS;

fn main() {
    println!("cargo:rerun-if-changed=build.rs");

    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR must be set"));
    let destination = out_dir.join("ca-certificates.crt");
    let mut pem = String::new();

    for certificate in TLS_SERVER_ROOT_CERTS {
        pem.push_str("-----BEGIN CERTIFICATE-----\n");
        let encoded = STANDARD.encode(certificate.as_ref());
        for line in encoded.as_bytes().chunks(64) {
            writeln!(
                pem,
                "{}",
                std::str::from_utf8(line).expect("base64 must be UTF-8")
            )
            .expect("writing to a String must succeed");
        }
        pem.push_str("-----END CERTIFICATE-----\n");
    }

    assert!(!pem.is_empty(), "Mozilla CA root set must not be empty");
    fs::write(&destination, pem).unwrap_or_else(|error| {
        panic!(
            "failed to write generated CA bundle to {}: {error}",
            destination.display()
        )
    });
}
