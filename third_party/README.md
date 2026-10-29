# Pinned upstream declarations

This directory contains **unmodified** declarations from three immutable AMS-GRA
upstream revisions. These are the public C++ interfaces implemented by this
provider, plus the declarations their include graph requires.

| Directory | Commit | Headers |
| --- | --- | ---: |
| `ir-mel/` | `8d9224519f12b44e0b28815755c56a32a28d24a0` | 61 |
| `common-mel/` | `f6908437d8fd2f7fb69896f9eb9cfd272d10c439` | 28 |
| `ams-math/` | `00be45190f0e47d268cece8b8c2f8fb58b5418d2` | 2 |

Each component also includes its original `LICENSE` and `INTENT.md`. The 97
files are listed in `manifest.json`, with source paths, Git blob IDs, SHA-256
checksums, and byte counts. No RF MEL, AMS VITA, Boost, commercial GigE SDK,
or upstream implementation binary is needed for this declaration slice.

Verify without accessing the network:

```sh
python3 scripts/vendor_upstream.py --verify
```

Explicitly restore the same locked files from their public upstream URLs:

```sh
python3 scripts/vendor_upstream.py --restore
```

For an offline restore, arrange the three complete source checkouts as
`/path/to/checkouts/{ir-mel,common-mel,ams-math}` and run:

```sh
python3 scripts/vendor_upstream.py --restore --source-dir /path/to/checkouts
```

Restoration validates all bytes before replacing existing files. It never
updates revisions or regenerates checksums. A dependency upgrade is a reviewed
source change: remeasure the declaration closure, retain notices, regenerate
the manifest from independently verified upstream blobs, and validate the
provider/client ABI together. Do not hand-edit these headers to make the build
pass. See [provenance](../docs/upstream-provenance.md).
