# Profiles

A profile is what makes a build of the firmware one product: the names the box shows the network (Wi-Fi setup network, host name, mDNS
instance and service type), whether relay-only is on until someone changes it, and optionally a front page of its own. It is chosen when the
firmware is built, not switched at run time.

`profiles/tonerelay` is the default. A product built on ToneRelay keeps its profile in its own repository and points the build at it:

```bash
TONERELAY_PROFILE_DIR=/path/to/my-product/profile cargo build --release   # in esp32-p4, with ESP-IDF's environment
touch sdkconfig.defaults                                                   # when switching profiles: makes the C component rebuild
```

A profile folder holds:

| File | |
|---|---|
| `profile.c` | Required. Defines `const tonerelay_profile_t TONERELAY_PROFILE` (see `include/tonerelay.h`). |
| `setup.html` | Optional. If present it is embedded, and `profile.c` can serve it as the front page by returning it from `setup_page`: the symbols are `_binary_setup_html_start` and `_binary_setup_html_end`. |

The relay contract (`docs/raw-relay-protocol.md`) is the same for every profile, so a client that speaks it works with all of them.
