# Rules for changes

## Layout

- One folder per server: `src/` (sources, internal headers), `tests/`, `Makefile`; data files in `data/`.
- Each server `Makefile` has `all`, `test`, `clean`, `print-bin`; builds into `build/` (`build-san/` with `SAN=1`). The top-level `Makefile` only loops over the folders; a new server is added to `SERVERS` there.
- `main()` stays in its own file so the tests link every other source.

## Code

- C is `-std=gnu11`, C++ is `-std=c++17`; `-Wall -Wextra -Wpedantic -Werror` with GCC and Clang.
- Format with clang-format 18 (`make format`; CI runs `make format-check` with 18.1.8).
- Comments and messages in English; Doxygen briefs for the declarations in headers.
- Keep each server's protocol and log strings: the integration tests depend on them.
- Linux only; dependencies are system packages (libevent, cmocka, GoogleTest).

## Tests

- C tests use cmocka, C++ tests GoogleTest; no home-made harness.
- Every fix comes with a regression test. Integration tests start the built binary on a free port (no fixed ports), stop it, and check that no processes or IPC objects are left.
- `make test` and `make test SAN=1` must pass with GCC and Clang.

## Commits and PRs

- Conventional Commits (`fix:`, `feat:`, `test:`, `refactor:`, `docs:`, `ci:`, `build:`), scoped by server: `fix(http-server): ...`.
- One topic per PR; PRs are squash-merged after CI is green. Every push to `main` gets the next patch tag and a release.
