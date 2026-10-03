# Security

These fixes are DLLs that games load in place of Windows' own, so it matters
that they do only what they say.

## What the DLLs do

- Load the real Windows DLL (`dinput8.dll` or `xinput1_3.dll`) only by its full
  `System32` path, never from the game folder or `PATH`.
- Pass every call through to it, changing only what the [README](README.md)
  describes: the reported controller ID (Splinter Cell: Conviction), stick
  inversion, and where the game's XInput calls point (Ghost Recon: Future Soldier).
- Write one log file next to themselves.

## What they don't do

- No network access, no registry changes, no other programs started, no files
  written other than the log.

## Release builds

Release DLLs are compiled by [GitHub Actions](.github/workflows/build.yml) from
the tagged source, and each release lists SHA256 fingerprints. For a public
repository, releases also carry a GitHub build attestation that you can check
with `gh attestation verify` (see the README).

## Reporting a problem

If you find a security problem, for example a way either DLL could be made to
load something other than the real Windows DLL, please report it privately with
**Security → Report a vulnerability** on this repository rather than in a
public issue.
