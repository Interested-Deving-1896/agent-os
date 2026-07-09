use std::fs::{remove_file, write, File};
use std::io::Read;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::path::PathBuf;
use std::process::{Command, Stdio};

fn child_mode(arguments: &[String]) -> Result<(), String> {
    let separator = arguments
        .iter()
        .position(|argument| argument == "--closed")
        .ok_or_else(|| "child arguments are missing --closed".to_string())?;

    for descriptor in &arguments[..separator] {
        let (fd, expected) = descriptor
            .split_once(':')
            .ok_or_else(|| format!("invalid descriptor expectation: {descriptor}"))?;
        let fd = fd.parse::<RawFd>().map_err(|error| error.to_string())?;
        let flags = unsafe { libc::fcntl(fd, libc::F_GETFD) };
        if flags < 0 {
            return Err(format!("fd {fd} is closed"));
        }
        if flags & libc::FD_CLOEXEC != 0 {
            return Err(format!("fd {fd} incorrectly retained FD_CLOEXEC"));
        }

        let mut contents = String::new();
        let mut file = unsafe { File::from_raw_fd(fd) };
        file.read_to_string(&mut contents)
            .map_err(|error| format!("read fd {fd}: {error}"))?;
        if contents != expected {
            return Err(format!("fd {fd}: expected {expected:?}, got {contents:?}"));
        }
    }

    for descriptor in &arguments[separator + 1..] {
        let fd = descriptor
            .parse::<RawFd>()
            .map_err(|error| error.to_string())?;
        if unsafe { libc::fcntl(fd, libc::F_GETFD) } >= 0 {
            return Err(format!("source fd {fd} leaked across spawn"));
        }
    }

    Ok(())
}

fn source_file(label: &str, suffix: &str) -> Result<(File, PathBuf), String> {
    let path = PathBuf::from(format!(
        "/tmp/fd-mapping-contract-{}-{suffix}",
        std::process::id()
    ));
    write(&path, label.as_bytes()).map_err(|error| format!("write {}: {error}", path.display()))?;
    let file = File::open(&path).map_err(|error| format!("open {}: {error}", path.display()))?;
    let flags = unsafe { libc::fcntl(file.as_raw_fd(), libc::F_GETFD) };
    if flags < 0
        || unsafe { libc::fcntl(file.as_raw_fd(), libc::F_SETFD, flags | libc::FD_CLOEXEC) } < 0
    {
        return Err("could not mark mapping source FD_CLOEXEC".to_string());
    }
    Ok((file, path))
}

fn command_for_child(expectations: &[(RawFd, &str)], closed: &[RawFd]) -> Result<Command, String> {
    let executable = std::env::args()
        .next()
        .ok_or_else(|| "argv[0] is unavailable".to_string())?;
    let mut command = Command::new(executable);
    command.arg("--child");
    for (fd, expected) in expectations {
        command.arg(format!("{fd}:{expected}"));
    }
    command.arg("--closed");
    for fd in closed {
        command.arg(fd.to_string());
    }
    command.stdin(Stdio::null());
    command.stdout(Stdio::piped());
    command.stderr(Stdio::piped());
    Ok(command)
}

#[cfg(target_os = "wasi")]
fn install_mappings(command: &mut Command, mappings: Vec<(OwnedFd, RawFd)>) -> Result<(), String> {
    use std::os::wasi::process::CommandExt as _;

    for (source, target) in mappings {
        let source_fd = source.as_raw_fd();
        command
            .fd_mapping(source, target)
            .map_err(|error| format!("map {source_fd}->{target}: {error}"))?;
    }
    Ok(())
}

#[cfg(unix)]
fn install_mappings(command: &mut Command, mappings: Vec<(OwnedFd, RawFd)>) -> Result<(), String> {
    use command_fds::{CommandFdExt as _, FdMapping};

    command
        .fd_mappings(
            mappings
                .into_iter()
                .map(|(parent_fd, child_fd)| FdMapping {
                    parent_fd,
                    child_fd,
                })
                .collect(),
        )
        .map(|_| ())
        .map_err(|error| error.to_string())
}

fn require_child_success(mut command: Command) -> Result<(), String> {
    let output = command.output().map_err(|error| error.to_string())?;
    if !output.status.success() {
        return Err(format!(
            "child failed with {:?}: {}",
            output.status.code(),
            String::from_utf8_lossy(&output.stderr)
        ));
    }
    Ok(())
}

fn sequential_collision() -> Result<(), String> {
    let (source_a, path_a) = source_file("A", "sequential-a")?;
    let (source_b, path_b) = source_file("B", "sequential-b")?;
    let source_a_fd = source_a.as_raw_fd();
    let source_b_fd = source_b.as_raw_fd();
    let target_b = source_a_fd.max(source_b_fd) + 1;
    let mut command = command_for_child(&[(source_b_fd, "A"), (target_b, "B")], &[source_a_fd])?;
    install_mappings(
        &mut command,
        vec![(source_a.into(), source_b_fd), (source_b.into(), target_b)],
    )?;
    let result = require_child_success(command).map_err(|error| {
        format!("fds source_a={source_a_fd} source_b={source_b_fd} target_b={target_b}: {error}")
    });
    let _ = remove_file(path_a);
    let _ = remove_file(path_b);
    result
}

fn swap_cycle() -> Result<(), String> {
    let (source_a, path_a) = source_file("A", "swap-a")?;
    let (source_b, path_b) = source_file("B", "swap-b")?;
    let source_a_fd = source_a.as_raw_fd();
    let source_b_fd = source_b.as_raw_fd();
    let mut command = command_for_child(&[(source_a_fd, "B"), (source_b_fd, "A")], &[])?;
    install_mappings(
        &mut command,
        vec![
            (source_a.into(), source_b_fd),
            (source_b.into(), source_a_fd),
        ],
    )?;
    let result = require_child_success(command);
    let _ = remove_file(path_a);
    let _ = remove_file(path_b);
    result
}

fn duplicate_target_rejected() -> Result<(), String> {
    let (source_a, path_a) = source_file("A", "duplicate-a")?;
    let (source_b, path_b) = source_file("B", "duplicate-b")?;
    let target = source_a.as_raw_fd().max(source_b.as_raw_fd()) + 1;
    let mut command = command_for_child(&[], &[])?;

    #[cfg(target_os = "wasi")]
    let rejected = {
        use std::os::wasi::process::CommandExt as _;
        command
            .fd_mapping(source_a.into(), target)
            .map_err(|error| error.to_string())?;
        command.fd_mapping(source_b.into(), target).is_err()
    };

    #[cfg(unix)]
    let rejected = {
        use command_fds::{CommandFdExt as _, FdMapping};
        command
            .fd_mappings(vec![
                FdMapping {
                    parent_fd: source_a.into(),
                    child_fd: target,
                },
                FdMapping {
                    parent_fd: source_b.into(),
                    child_fd: target,
                },
            ])
            .is_err()
    };

    let _ = remove_file(path_a);
    let _ = remove_file(path_b);
    if rejected {
        Ok(())
    } else {
        Err("duplicate child descriptor mapping was accepted".to_string())
    }
}

fn parent_mode() -> Result<(), String> {
    sequential_collision().map_err(|error| format!("sequential collision: {error}"))?;
    println!("sequential_collision=ok");
    swap_cycle().map_err(|error| format!("swap cycle: {error}"))?;
    println!("swap_cycle=ok");
    duplicate_target_rejected().map_err(|error| format!("duplicate target: {error}"))?;
    println!("duplicate_target=ok");
    Ok(())
}

fn main() {
    let arguments = std::env::args().skip(1).collect::<Vec<_>>();
    let result = if arguments.first().map(String::as_str) == Some("--child") {
        child_mode(&arguments[1..])
    } else {
        parent_mode()
    };
    if let Err(error) = result {
        eprintln!("fd_mapping_contract: {error}");
        std::process::exit(1);
    }
}
