# ADR-0001: One Runtime, Three Least-Privilege Profiles

**Status:** accepted

Bonded Inbox uses one C++20 Logos Core module and a versioned skill registry.
Inbox, Vault, and Settlement deployments load distinct profile manifests and
data directories. Capabilities are denied unless explicitly present in the
profile. This satisfies the three-agent product architecture without creating
three drifting runtimes.

The public module boundary accepts and returns canonical JSON strings so schema
changes do not leak unstable upstream C++ types into the domain. Adapters own all
Storage, Delivery, and LEZ calls.
