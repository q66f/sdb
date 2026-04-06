bin := "target/debug/sdb"

@default:
    cargo build && ({{bin}} -l; {{bin}} -ctest -dtest64 apple; {{bin}} -den src)

@b:
    cargo build

@t *a:
    cargo build && {{bin}} {{a}}

@s *a:
    cargo build && strace {{bin}} {{a}}

@install:
    RUSTFLAGS="-C strip=symbols" cargo install --path . --root /usr/local/
