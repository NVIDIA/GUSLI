# Security Policy

## Reporting a Vulnerability

If you believe you have found a security vulnerability in Gusli, please report it
responsibly. **Do not open a public issue.**

- Preferred: NVIDIA Vulnerability Disclosure Program — https://www.nvidia.com/en-us/security/
- Email the NVIDIA PSIRT at **psirt@nvidia.com**. For sensitive reports, encrypt
  with the NVIDIA PSIRT PGP key published on the page above.
- If you have access to this GitLab project, you may also use **GitLab confidential
  issues / private vulnerability reporting** so the report is visible only to
  maintainers.

Please include a description, affected files/versions, and reproduction steps.
We will acknowledge and coordinate a fix and disclosure timeline with you.

## Security Architecture & Context

**Project.** Gusli (G3+ User Space Access Library) is a C++ library that provides
user-space, kernel-bypass I/O (read/write) to local block devices or files. It ships
a client shared library (`.so`) for I/O-submitting applications and a server shared
library (`.so`) that executes those I/Os against a backend (e.g. an SPDK bdev). Client
and server run as **separate user-space processes on the same machine** and communicate
over a Unix domain socket for control-plane messaging (`00utils/utils.cpp`,
`01common/cp_messaging.hpp`) and **shared memory for zero-copy data-plane transfers**
(`01common/dp_io_ring.hpp`). Optional TCP/UDP transports also exist in the socket layer.

**Software classification.** Internal system-software library / SDK (storage data-path
component), not a standalone network service.

**Repository Exposure Classification: Internal.** Hosted on gitlab-master.nvidia.com as scratch for GitLab visibility 'External'; Apache-2.0, NVIDIA copyright. Public/external hosting: git@github.com:nvidia/gusli.git

**Service Exposure Classification: Internal-Isolated (confidence: medium).** Basis:
the documented deployment model is two cooperating processes on a single host that
talk over a Unix domain socket and a shared-memory I/O ring; there is no external
network listener in the default/local configuration. The confidence is *medium* rather
than *high* because the socket layer (`00utils/utils.cpp`) also implements TCP/UDP
transports, so a deployment could expose the server beyond the local host — that
scenario is out of scope for this classification and should be reviewed separately.

**Primary security responsibility.** The repository owner/creator, **Daniel
Herman-Shmulyan (@danielhe)**, and the Gusli/Excelero storage team.

**Key boundaries & interfaces (real files).**
- Control-plane message handling and client handshake/auth:
  `03backend/server_imp.cpp` (`hello` handler), `01common/cp_messaging.hpp`.
- Trust boundary between untrusted client input and the server:
  `01common/dp_io_ring.hpp` (scatter-gather list parsing from shared memory).
- Local IPC endpoint (Unix domain socket) creation & permissions:
  `00utils/utils.cpp`.
- Client-side config parsing (device/connection strings): `05clnt/client_imp.cpp`.
- Public API surface: `gusli_client_api.hpp`, `gusli_server_api.hpp`.

## Threat Model

Threats derive from the actual code and from an Argus audit
(`a295a97d-387c-4f80-a937-90a793f0fae6`, commit `87f1929b`). Each notes whether it is
fixed here or deferred to the owner.

1. **Trivial client authentication (CRITICAL, DEFERRED).**
   `03backend/server_imp.cpp:273` only checks `security_cookie[0] != 0` in the `hello`
   handler. Any local process that presents a non-empty first byte is accepted, so the
   cookie provides no real authentication. Fixing this requires an authentication
   redesign (constant-time comparison against a provisioned secret, or OS-level peer
   credential checks via `SO_PEERCRED`) — a product/credentials change that is **not a
   safe mechanical fix**. Flagged for the owner as urgent.

2. **Heap buffer over-read from crafted scatter-gather list (HIGH, DEFERRED).**
   In `01common/dp_io_ring.hpp:192-193`, a multi-range I/O is copied with
   `memcpy(nm, mm, sgl_len)` where `sgl_len` comes from client-controlled shared memory,
   then the loop iterates `nm->n_entries`. If `n_entries` is inconsistent with `sgl_len`,
   the loop reads past the allocation. A correct fix must validate `n_entries` against
   `sgl_len`/allocation bounds — this touches data-path/protocol semantics and is
   deferred to the owner rather than guessed at.

3. **TOCTOU / symlink attack on the Unix domain socket (MEDIUM, DEFERRED).**
   `00utils/utils.cpp` `unlink()`s then `bind()`s and `chmod()`s the socket path
   (lines 154, 177) with no protection against a path component being swapped for a
   symlink between operations. Hardening (private per-instance directory, `O_NOFOLLOW`
   semantics) changes socket-setup behavior and is deferred.

4. **World-accessible socket permissions 0777 (MEDIUM, DEFERRED).**
   `00utils/utils.cpp:151-154` chmods the UDS to `rwxrwxrwx`. The inline comment shows
   this is **intentional** ("Allow sudo app to talk to non-sudo app"), so tightening it
   would break the documented cross-privilege IPC model. Left for owner decision (e.g.
   group-scoped `0770` + shared group) rather than a blind mechanical change.

5. **Security cookie stored/transmitted in plaintext (MEDIUM, DEFERRED).**
   The cookie is a plain `char[]` in `gusli_client_api.hpp:72` and
   `01common/cp_messaging.hpp:74`. Improving confidentiality is a design/crypto change,
   deferred to the owner. Closely related to threat 1.

## Critical Security Assumptions

- Gusli assumes client and server run on the **same trusted host** as cooperating
  local processes; it is **not** hardened as a multi-tenant or network-exposed service.
- The `security_cookie` handshake is assumed, today, to provide only marginal
  gate-keeping — it must not be relied on for real authentication (see threat 1).
- Callers are assumed to pass structurally valid scatter-gather lists; the server does
  not currently fully validate untrusted `n_entries` (see threat 2).
- The local Unix domain socket path and its parent directory are assumed to reside on a
  trusted, non-attacker-writable filesystem location (see threats 3–4).

## Scope

This policy and the accompanying Argus-derived threats cover the Gusli source in this
repository (client/server libraries, common transport and data-path headers, utilities,
and examples). It does **not** cover downstream applications that link Gusli, the SPDK
backends themselves, or non-default TCP/UDP network deployments, which require their own
review.
