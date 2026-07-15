# assets — Celeste benchmark cart

The Gate #2 real-cart benchmark (`../firmware/pico-e32-luabench/bench/celeste_bench.cpp`) runs
**Celeste Classic** (`celeste.p8`, by Maddy Thorson & Noel Berry) headless on z8lua.

**These are not committed.** `celeste.p8` and the generated `celeste_cart.h` are third-party
game code — they are `.gitignore`d and must not be redistributed. Only this README and the
generator script are tracked.

## Setup (after cloning, before building)

```sh
python3 gen_celeste_cart.py
```

This fetches `celeste.p8` from [DavidVentura's PicoPico](https://github.com/DavidVentura/PicoPico)
(the same copy our ~15.8 ms/frame number is measured against) and writes `celeste_cart.h`
(embedded Lua source + map + sprite-flags), which `celeste_bench.cpp` includes.

If offline, drop a `celeste.p8` here manually and re-run the script.
