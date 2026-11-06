# Contributing to FMini-1

Thanks for helping! To keep things easy to follow, please use the right place:

| What | Where |
|---|---|
| A bug you can reproduce | [Issues](https://github.com/ruppert/fmini-1/issues/new/choose): one issue per bug |
| A feature idea or request | [Discussions › Ideas](https://github.com/ruppert/fmini-1/discussions/categories/ideas) (no promise it will be added) |
| How to use it, or how something works | [Discussions › Q&A](https://github.com/ruppert/fmini-1/discussions/categories/q-a) |
| Forks, mods, music, demos | [Discussions › Show and tell](https://github.com/ruppert/fmini-1/discussions/categories/show-and-tell) |
| Anything else | [Discussions › General](https://github.com/ruppert/fmini-1/discussions/categories/general) |

Feature requests posted as issues will be moved to Discussions or closed.

## Reporting a bug

Please include the FMini-1 version (shown on the ABOUT screen: press GLO and choose ABOUT), your computer's
OS and browser if the editor or installer is involved, the steps to reproduce it, what you expected and what happened.
A photo or a short video helps.

## Pull requests

Pull requests are welcome, including AI-assisted ones, with the same bar for all:

- keep it small and focused on one thing;
- make sure you understand the change and can explain it;
- test it on a real FM-1;
- run `tests/run_tests.sh` (see [BUILDING.md](BUILDING.md); the sections listed in [CLAUDE.md](CLAUDE.md) as failing
  before your change are known and not yours to fix);
- new files carry the GPL-3.0-only SPDX header.

By contributing you agree that your contribution is licensed under GPL-3.0-only.

FMini-1 is a fork of [Felucca](https://github.com/hugelton/Felucca) by Leo Kuroshita. The engines in `firmware/engines/`
(a git submodule: ANALOG, LOFI, TRIO, WHEEL and NOISE, and the code they share) live in
[Felucca-Engines](https://github.com/hugelton/Felucca-Engines), under the MIT licence: changes to them go there, as
pull requests to that repository. Changes to Felucca itself go to its own repository.

The web editor, the installer and the browser emulator are part of this repository (`web/`, published to
[the site](https://ruppert.github.io/fmini-1/) from `docs/`): pull requests for them are made here too.
