# Contributing

Thanks for helping! Bug reports, compatibility reports ("program X does / doesn't work") and pull requests are all
welcome.

## Reporting a problem

Open an issue and use the bug report template. The most useful thing you can add is the `MoofDiag.out` file: run
**MoofDiag** (from the release's tools download) right after the problem, while the Mac is still running.

## Building and testing

You only need Docker for the Mac builds, and gcc plus CMake for the host tests. No Mac, emulator or Apple software is
needed for either.

```sh
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure            # full configuration
cmake -S . -B build-lite -DMT_NO_DHCP=ON && cmake --build build-lite
ctest --test-dir build-lite --output-on-failure       # MoofTCP Lite configuration
scripts/build-mac.sh MoofTCP                          # the 68k build (Retro68 in Docker)
scripts/gate-check.sh                                 # every Mac target must compile with -Werror
```

The emulator (Snow) tests need your own Mac ROMs and Apple system software; they are optional for contributors.
Real-hardware testing is done by the maintainer before a release.

## Pull requests

- Keep a change focused, and run the host tests (both configurations) first.
- `core/` rules: no `malloc`, no global state, all packet access through `mt_rd16/32` and `mt_wr16/32`, wrap-safe
  clock and sequence compares, no blocking. Add a test for new behavior. `CLAUDE.md` has the details (it is written for
  coding agents but is a good map for people too).
- Memory is a feature: resident size must stay below MacTCP's. Say in the PR if your change grows the driver.
- **Do not attach or commit Apple software** (ROMs, System disks, MacTCP, disk images) or built binaries.
- AI-assisted contributions are fine; you are responsible for understanding and testing what you submit.

By contributing you agree that your work is released under the repository's [MIT license](LICENSE).
