# ChatConnect — 20 actual source files

**Exactly 20 tracked source files.** No ZIP/tar/encoded archive inside this project; Docker does not unpack original source files. 37 original images and icons are converted to binary data **embedded directly into C++ source** (`web/static_assets.hpp`), and served at their original URLs by `server.cpp`. The other live HTML/CSS/JS, C++ and database logic are retained.

This is a **source consolidation**, not 20 independent features. The build generates executable files in Docker layers, but it does not recreate the former 104-file tree. The 20-file edition is a separate candidate: **validate live Docker tests and two-account functionality before deployment**.

## Team/module ownership (five files per member)

### Thanushka — Frontend / UI-UX — 5 files
- `web/index.html`
- `web/style.css`
- `web/manifest.webmanifest`
- `web/static_assets.hpp`
- `web/pwa-icon-192.png`

### Lahari — C++ backend / security — 5 files
- `server.cpp`
- `socket.hpp`
- `CMakeLists.txt`
- `tests/auth_tests.cpp`
- `.env.example`

### Harshitha — Messaging / calls — 5 files
- `web/app.js`
- `web/cc-calls.js`
- `web/cc-emoji-data.js`
- `web/cc-voice-emoji.js`
- `web/sw.js`

### Member 4 — DevOps / backups — 5 files
- `Dockerfile`
- `compose.yaml`
- `.gitignore`
- `README.md`
- `ops/operations.ps1`

**Authorship:** File assignment means module responsibility for future work, **not historical proof that a person wrote the file**. Git commits must use truthful author/co-author attribution for actual contributions; ideas/testing may be documented explicitly. Do not falsify contribution history.

## Local runtime verification

1. Copy `.env.example` to `.env` and replace the placeholder with a strong unique password.
2. Do **not** start the new build against your existing working `chatconnect` Compose project or volume.
3. In a fresh directory use `docker compose -p chatconnect20test -f compose.yaml up -d --build`; first edit `.env` to use `CHATCONNECT_HOST_PORT=18085` and `CHATCONNECT_BIND_IP=127.0.0.1`.
4. Check `docker compose -p chatconnect20test ps` and GET `http://127.0.0.1:18085/health`.
5. Test login/signup, two-account private messaging, groups, typing, uploads, calls, themes, PWA/mobile, database persistence, and access denial for other users. Do not point to any production data until fully tested.

## Native compile check

Build C++17 with PostgreSQL and OpenSSL (`libpq`, `libcrypto`). On Debian:

```sh
g++ -std=c++17 -O2 -pthread -I/usr/include/postgresql server.cpp -o ChatConnectWeb -lpq -lcrypto
g++ -std=c++17 -O2 -pthread -I/usr/include/postgresql tests/auth_tests.cpp -o ChatConnectTests -lpq -lcrypto
./ChatConnectTests
```

## Operation and deployment notes

- `ops/operations.ps1 -Action backup` — existing backup logic, optional, local Docker only.
- `ops/operations.ps1 -Action restore-test -BackupFolder "..."` — isolated restore test.
- `ops/operations.ps1 -Action schedule` / `remove-schedule` — optional local task.
- The old extensive PowerShell/JS integration test harness is **not included** in these 20 files. Use the archived external test kit against this running edition. Its absence is a source/testing limitation, not an assertion of test equivalence.
- Local Compose binds loopback only. Do **not** deploy it as-is: plan production HTTPS reverse proxy, external-network TURN for WebRTC, firewall policy, strong secrets, persistent PostgreSQL/media volumes, disk/backups, health/monitoring and abuse controls. Message transport is not end-to-end encrypted.
- Cloud storage and compute free tiers have quotas, capacity and availability limits.
