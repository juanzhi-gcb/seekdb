# Format03 packed immutable pages

Format03 extends the existing immutable page tree; page size remains 4096 bytes. New SQL databases use the selected default in Options; Store's low-level default remains format02. Existing format02 files always remain format02 even when packing is requested. Existing format03 files can be opened with packing disabled and continue to read old packed values. No migration or reclamation is implemented.

## Encoding and validation

Root and page magic are MVDBRT03 and MVDBPG03. An independently valid root must have recognized magic, correct checksum, valid extent and root bounds. Two valid roots with differing versions are rejected; an invalid candidate may be ignored during fallback. Old format02 binaries reject format03 at startup (their generic error says corrupt file).

Type3 pages hold a slot count at byte12, zero reserved bytes16..23, the existing checksum field at24, and an 8-byte little-endian offset/length directory beginning at32. Payload is packed canonically from the end of the page backwards; directory/payload bounds, adjacency, count and nonoverlap are checked. At most508 slots fit. Empty values are supported.

Only format03 leaf references at depth15 may contain bit63. Such references encode `(1ULL<<63) | (page_id<<9) | slot`; slots508..511 are invalid. Physical addresses must be within committed/working extent and precede the referencing parent. Internal links, roots and overflow-next pointers cannot carry tags. Ordinary overflow values keep their previous representation inside format03.

## Write and recovery rules

A bounded mutation batch packs fitting values, flushes all data pages, then writes index parents. The working root changes only after the complete merge succeeds. Failed partial pages are not cached. A page with only one value falls back to an ordinary value record; old format03 packed singleton pages remain readable. Oversized values use ordinary overflow chains. This avoids extra packed-page management when density cannot improve.

The production commit protocol is unchanged: synchronize data, write the new root record, synchronize again, then acknowledge. Statement savepoints, transaction rollback and restart selection remain common to packed and ordinary values. Appending new versions does not reclaim historical pages.

## Scan cache

Each scan owns a lazy fixed-size direct-mapped cache of validated immutable packed pages, destroyed at scan completion or exception. SQL defaults to1MiB; the low-level Store scan budget defaults to256KiB. Entries include a4096-byte page and8-byte identifier (255 entries,1046520 bytes for1MiB on the tested ABI). Fixed object/allocation overhead, the current returned row, index recursion and nested scans are additional. This is not a process memory cap.

A cache hit still validates the requested physical address and slot. The row is copied into an owned string before a callback can mutate the store or start another scan. No mutable/incomplete page enters this cache. Zero budget disables reuse. Index cache and packed scan cache are separate.

Full SQL row validation remains in force: filtered and unprojected fields are checked for valid UTF-8, finite float32, dimensions and boundaries. Page-skipping vector summaries are therefore not silently introduced by this format change.
