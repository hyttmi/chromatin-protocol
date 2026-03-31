# chromatindb

A decentralized, post-quantum secure database node with signed blob storage and peer-to-peer replication.

**Current release: v1.5.0**

chromatindb is a standalone daemon that stores cryptographically signed blobs organized into namespaces. Every blob is verified before storage, encrypted in transit using post-quantum algorithms, and replicated automatically across connected peers. Nodes form a self-organizing network where data is censorship-resistant and cryptographically authenticated end-to-end.

See [db/README.md](db/README.md) for full documentation including build instructions, configuration reference, protocol overview, and deployment scenarios.

See [Python SDK](sdk/python/README.md) for the pip-installable client library.

## License

MIT
