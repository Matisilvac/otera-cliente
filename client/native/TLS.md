# TLS rehearsal builds

The `codex/tls-client-hardening` branch adds verified TLS to the native `Connection`
used by both login and game traffic. `OTERA_TLS_SERVER_NAME` opts in; failed trust,
hostname validation, or negotiation never falls back to plaintext. A packaged
TLS release must require its launcher/profile before accepting credentials.
`OTERA_TLS_CA_FILE` supplies additional trust and `OTERA_RSA_PUBLIC` sets the
protocol 860 server identity. These settings contain no account password.

CI checks out the pinned OTClient 4.1 and vcpkg revisions, applies the existing
autowalk patch plus the TLS and diagnostic patches, and builds x64 Windows/Linux.
Six tests execute that same binary against disposable local servers: TLS 1.2,
TLS 1.3, wrong name, unknown CA, plaintext peer, and missing CA file. The diagnostic
accepts only a loopback port and exchanges synthetic bytes; it loads no Lua,
assets, accounts or game world. Artifact collection requires the tested binary's
SHA256 to match and reverse-checks the applied patches.

Both native builds and all six cases passed in
[run 35883977137](https://github.com/Matisilvac/otera-cliente/actions/runs/35883977137).
The transport tests also passed on the local arm64 Mac build. This is not full
Windows/Linux GUI or game-session QA. Artifacts are rehearsal binaries only;
this branch does not publish a release or update players automatically. It does
not provision certificates or change the live server's network routing.

Game assets must remain in `data/things/<version>` and `data/sounds/<version>`.
Transport CI does not install or modify assets. The separate private packager
keeps those runtime paths; full packaged game validation remains necessary.
