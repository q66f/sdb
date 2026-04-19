bin := "target/debug/sdb"

@default:
    cargo build && ({{bin}} -l; {{bin}} -den src; {{bin}} -den "apple p")

@b:
    cargo build

@t *a:
    cargo run -- {{a}}

@s *a:
    cargo build && strace {{bin}} {{a}}

@install:
    cargo install --path . --root /usr/local/
