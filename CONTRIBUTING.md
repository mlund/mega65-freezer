# Contributing

Contributions of code, tests, documentation, and bug reports are all welcome.

## Ground rules

- Be respectful and constructive. Assume good faith.
- Keep changes focused: one logical change per pull request.
- Discuss large or invasive changes in an issue before writing the code.

## Licensing and origin

By contributing, you agree that your contributions are licensed under project
licenses.

If you port or adapt code from another project, say so in the file and preserve
the original attribution and license notice. Do not paste code whose license is
unknown or incompatible.

## Development workflow

Requirements: a recent stable Rust toolchain (see `rust-version` in
`Cargo.toml`).

Before opening a pull request, run tests incl. with the Xemu harness.

- Follow test-driven development: write a failing test that pins the behaviour,
  then make it pass. Expected values come from the spec, real examples, or
  literals — never from re-implementing the logic inside the test.
- Comments explain *why*, not *what*.

See [`AGENTS.md`](AGENTS.md) for the full design and testing conventions.

## Using AI coding assistants

Contributions written with the help of AI coding agents (Claude Code, Codex, and
similar) are welcome, **provided a human is in the loop**. If you use one:

- You, the human contributor, are responsible for the change. Read, understand,
  and stand behind every line you submit — the same bar as code you wrote by
  hand.
- Verify it: run the build, tests, `cargo fmt`, and `cargo clippy` locally.
  "The model said so" is not a substitute for evidence.
- Ensure the agent did not import code, comments, or data of unknown or
  incompatible provenance (see *Licensing and provenance* above).
- Agents should follow the conventions in [`AGENTS.md`](AGENTS.md).

Unreviewed, bulk machine-generated pull requests will be closed.

## Submitting changes

1. Fork and create a topic branch.
2. Make your change with tests and a brief, clear commit message.
4. Open a pull request describing *what* changed and *why*.
