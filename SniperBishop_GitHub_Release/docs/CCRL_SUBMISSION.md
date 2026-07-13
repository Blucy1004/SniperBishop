# CCRL testing request checklist

CCRL is an independent volunteer engine-testing group. A public repository does not automatically produce a rating; a tester must accept and run the engine.

## Prepare the release

Publish a tagged release such as:

```text
v2.61-hybrid-alpha
```

Attach one ZIP containing:

```text
SniperBishop.exe
firstnet_v3.snnue
README.txt
LICENSE.txt
```

Before uploading, test the extracted ZIP in a new empty folder.

## Required practical information

Include these details in the release notes and testing request:

- Engine name: SniperBishop
- Version: 2.61 Hybrid Tuned
- Author/credit: Blucy1004
- Protocol: UCI
- Platform: Windows x64
- CPU requirements: x86-64; state explicitly if AVX2 is required
- Threads: single-threaded unless the engine says otherwise
- Ponder: off
- Hash option: supported
- Required companion file: `firstnet_v3.snnue`, beside the executable
- Recommended options: defaults
- Source/release location
- License
- Contact method
- SHA-256 checksums

## Ready-to-send request

Subject:

```text
New UCI engine for testing: SniperBishop 2.61 Hybrid Tuned
```

Message:

```text
Hello,

I would like to submit SniperBishop 2.61 Hybrid Tuned for possible CCRL testing.

SniperBishop is a free, open-source Windows x64 UCI engine written in C++17.
The current release uses a classical/FIRST_NET hybrid evaluator.

Engine: SniperBishop 2.61 Hybrid Tuned
Author: Blucy1004
Protocol: UCI
Platform: Windows x64
Ponder: off
Threads: 1
Required network: firstnet_v3.snnue, placed beside the executable
Recommended settings: default UCI options
Source and binary release: <GITHUB RELEASE URL>
License: MIT

The release archive includes SHA-256 checksums and has been tested after extraction in a clean folder.

Thank you for considering it for testing.
```

## Important

- Do not call a local CuteChess estimate an official CCRL rating.
- Keep the exact tested binary and network downloadable after publication.
- Do not silently replace a release asset; publish a new version/tag.
- If a tester reports a crash or UCI issue, preserve the broken release and issue a new fixed tag.
