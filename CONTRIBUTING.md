# Contributing

How work on the engine is done: branches, building and testing, code style, comments, docs and commits.

- [Branches and pull requests](#branches-and-pull-requests)
- [Before you push](#before-you-push)
- [Code style](#code-style)
- [Comments](#comments)
- [Tests](#tests)
- [Documentation](#documentation)
- [Commit messages](#commit-messages)
- [Adding a dependency](#adding-a-dependency)
- [Reporting bugs and asking for features](#reporting-bugs-and-asking-for-features)

## Branches and pull requests

- `main` is the last known-good state. It only moves forward from `develop` when CI is green.
- `develop` is where work lands.
- Larger changes go on a branch off `develop` (`feature/text-outlines`, `fix/hot-reload-bindings`) and come back
  through a pull request into `develop`.

A pull request should do one thing, build on all three platforms in CI, include tests for new behavior, and update the
docs it affects. The pull request template has the checklist.

## Before you push

Run what CI runs:

```bash
cmake --preset dev -DPH_WARNINGS_AS_ERRORS=ON
```

```bash
cmake --build --preset dev
```

```bash
ctest --preset dev
```

If you touched rendering, run the samples and look at them, including a screenshot run:

```bash
./build/dev/samples/isometric/isometric --night --frames 60 --screenshot night.png
```

If you touched hot reload, the ECS's storage or scheduling, run the sprites sample, rebuild `sprites_code` while it
runs, and check the log line saying the world was restored.

CI (`.github/workflows/ci.yml`) builds `dev` and `release` on Windows, macOS and Linux, runs the tests, and renders the
isometric sample on Direct3D 12 on Windows. A change isn't done until all six jobs pass.

## Code style

Formatting is `.clang-format` (LLVM-based, 4-space indent, 120 columns). Run `clang-format -i` on the files you change.

Beyond formatting:

- C++20 without compiler extensions. No exceptions, and nothing that needs RTTI.
- `snake_case` for functions, variables and files, `PascalCase` for types, `PH_UPPER_CASE` for macros. Members end
  in `_`. Use the short integer aliases (`u32`, `f32`, `usize`).
- Headers include what they use. Forward-declare SDL types in public headers where you can.
- Assert on programming errors. For runtime failures (missing files, bad data), log and carry on; bad content should
  never crash the game. See [conventions](docs/reference/conventions.md#errors).
- Don't allocate in per-entity or per-sprite loops. Reuse buffers across frames.
- Keep things deterministic: no unseeded randomness, and don't let the order of an unordered container change results.
- Platform-specific code stays behind SDL. If you really need an `#ifdef`, keep it inside one `.cpp` file.
- CI treats warnings as errors on Linux and macOS. If you silence one, leave a comment saying why it's wrong.

## Comments

Write comments for someone who knows C++ but hasn't seen this code. Explain what the code is for and why it's done
this way, not what each line does: `// Swap-remove: order doesn't matter for particles.` helps, `// Increment i`
doesn't.

Public types and functions get a comment unless the name already says it all. Start with what it does, then add when
to use it, what it costs or what can go wrong, if that's worth knowing. A short usage example in a header is fine;
indent it four spaces after the `//`.

Keep it plain. No banner lines or ASCII boxes; a section label is just a one-line comment followed by a blank line.
When you change code, fix the comments around it too, because a wrong comment is worse than none. TODOs should say
what's missing and, if you know, why: `// Not yet: kerning across font fallbacks.`

## Tests

Tests use [doctest](https://github.com/doctest/doctest) and live in `tests/test_<area>.cpp`. One `ph_tests` executable
holds all of them.

- New behavior gets a test. Bugs get a test that fails before the fix.
- Name test cases as `"<area>: what's being checked"`, for example `"input: a tap is seen once per frame and once per
  fixed tick"`.
- Tests can't assume a GPU or a window. Code that needs one should be split so its logic can be tested without it (the
  font, tilemap and sprite sheet tests work this way).
- Data files go in `tests/data/`, with a note in its README on how they were made. Keep them tiny.

## Documentation

- The headers document the API. Update the comment when you change a function.
- The guides in `docs/guides/` explain how to use each system. If you add a feature, add it to its guide; if you add a
  system, add a guide and link it from `docs/README.md`.
- `docs/tutorial.md` follows `samples/tutorial/main.cpp`. Change them together.
- Code in the docs must compile against the current API. If an example is more than a few lines, consider putting it
  in a sample or a test instead and linking to it.
- Write like the existing docs: second person, present tense, plain words, no marketing. The engine is not compared to
  other engines anywhere in the repository, the docs or GitHub.
- Add a line to `CHANGELOG.md` under **Unreleased** for anything a game developer would notice.
- Screenshots go in `docs/images/` (PNG for flat UI, JPEG at quality 85 for game scenes, 1280 pixels wide).

## Commit messages

- First line: what the commit does, in the imperative, under about 72 characters. "Font atlases upload only new
  glyphs", not "Updated text.cpp".
- Then a blank line and, when it isn't obvious, *why*: the problem, the approach, anything surprising.
- One logical change per commit where practical; a commit should build and pass tests on its own.
- No generated trailers or tool attributions.

## Adding a dependency

Think twice: every dependency is code everyone builds, on every platform. If it's worth it:

1. Add it to `cmake/PhDependencies.cmake` with `ph_fetch(name <release archive URL> <sha256>)`, pinned to a release.
2. Mark it `SYSTEM` (the helper does) so its warnings don't fail our builds.
3. Note the version, purpose and license in [docs/reference/cmake.md](docs/reference/cmake.md#dependencies) and in the
   README's license section.

## Reporting bugs and asking for features

Use the issue templates on GitHub. For bugs, the most useful things are: the platform and GPU backend (shown at startup
and in the overlay), the log, steps to reproduce, and a screenshot if it's visual (`--screenshot` makes one).
