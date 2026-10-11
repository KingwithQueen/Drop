# csrp provenance

Upstream: https://github.com/cocagne/csrp/tree/rfc5054_compat
Revision: b1a6ebc3331672694149469de6f33ce3c69e1fe9
License: MIT (LICENSE). The five upstream files are unmodified.

SHA-256:

| File | Hash |
| --- | --- |
| LICENSE | 36bc0d489d860a72d45c7b518606b35c31492fe3b045fa1084cfc7f9af3f9b7c |
| README.md | 6d5afe5e9e2e56377672a58c7fb9a474b03b5d585a5941225fbf21be13ed12d7 |
| srp.c | a0d12ded992cb99b78781d9ef10e065fa814542265cdc3baf7eefe70117de234 |
| srp.h | c50752fa0dee835244541278072417c0b6c6e58bec1f692a48b98930a2e4c4a8 |
| test_srp.c | 24abccd3911af26bdd38f14b654d80bd86d17536bc46d29a1c68eb727113a977 |

Drop compiles srp.c via dropcommon/srp_vendor.c. That wrapper redirects proof comparison to CRYPTO_memcmp, modular exponentiation to BN_mod_exp_mont_consttime, random integers to checked BN_priv_rand (minimum 256 bits), BIGNUM cleanup to BN_clear_free, and heap cleanup to zeroing/free. OpenSSL modern RNG is initialized once rather than using upstream platform seeding. Allocation, random and exponentiation failures fail closed. Deprecated SHA-context warnings are disabled only for this upstream translation unit. Fixed public values are checked as 0 < value < N before calling upstream code. The wrapper increases upstream's 32-bit salt to 256 bits. Application passwords are prehashed with Argon2id. This wrapper and Drop's record/channel integration have not received independent security review.
