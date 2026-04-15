# CLI Backlog

## Groups
Local contact groups in SQLite. `group create/list/add/rm` commands.
`--share <group_name>` resolves all members' KEM pubkeys as envelope recipients.
No protocol changes — purely client-side convenience.

## Chunked file upload (> 500 MiB)
Split large files into chunks, each independently envelope-encrypted.
Requires bigger chunk size (10-50 MiB) and request pipelining for performance.
Needs CPAR magic on chunks for node-side list filtering.

## Streaming envelope encryption
Eliminate full-file memory buffering. Read file in chunks, encrypt segments
on the fly, stream to node via chunked transport. Blocked by FlatBuffer
requiring full blob data for serialization.

## Request pipelining
Send multiple ReadRequests without waiting for each response.
Would significantly speed up multi-blob downloads.
