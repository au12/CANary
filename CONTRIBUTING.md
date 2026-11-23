# Contributing

Thank you for looking. The most useful contributions, in order:

1. **Trying it on a car and reporting what happened.** Attach the file from
   **Device > Advanced > What this car answers**. It names the model, not the car.
2. **Vehicle profiles**: a new car, or corrections to an existing one. See
   [docs/profiles.md](docs/profiles.md).
3. **Checks**: a new one, or better limits for an existing one, with where the numbers come
   from.
4. **Code.** See [docs/development.md](docs/development.md).

## Before opening a pull request

```
cmake -S host -B build-host -G Ninja && cmake --build build-host
build-host/canary-tests
python3 tools/gen_checks_doc.py
```

- The tests pass, and a new behaviour has a test.
- `docs/checks.md` is regenerated if a profile's checks changed.
- Changes to how checks are evaluated, to the log format or to the expression language come
  with the matching change in [Glovebox](https://github.com/au12/glovebox). The reason is in
  [docs/development.md](docs/development.md#keeping-glovebox-in-step).
- Text that people will read is plain: short sentences, no jargon, no abbreviations a driver
  would not know.

## Two rules that do not bend

- **CANary only reads.** Nothing that clears codes, runs actuator tests, codes, adapts or
  programs a control unit will be merged, in any form, including behind a setting.
- **No data leaves the owner's network** except to the server they entered.

## Licensing

Contributions are accepted under the project's license, GPL-3.0-or-later. Vehicle data taken
from another source must say where it came from and be under a license that allows it; see
[THIRD-PARTY.md](THIRD-PARTY.md).
