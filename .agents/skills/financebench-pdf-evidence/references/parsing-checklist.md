# Parsing checklist

Review list for turning one filing PDF into page-level sources.

## Identity

- [ ] `document_id` is non-empty and carries no leading or trailing whitespace.
- [ ] The document digest is the SHA-256 of the **whole input file**, lowercase
      hex, and is stored on every page record.
- [ ] The page identity is derived from `(document_id, digest, page_number)`, so
      a revised PDF produces new identities while old citations still resolve to
      the revision they were captured from.
- [ ] The identity is stable for the same inputs and stays inside the source-id
      length limit (256 characters is the common ceiling).

## Page coverage

- [ ] Every physical page appears exactly once, in order, starting at 1.
- [ ] Blank pages are present with empty text — they are not removed, merged or
      renumbered.
- [ ] A page with no extractable text is reported as empty, not replaced by a
      placeholder carrying only the page number.
- [ ] No page is silently truncated to fit an entry limit. If a page is too
      large for the store, that is an ingestion problem to surface, not a reason
      to drop text.

## Failure handling

- [ ] Empty input, a non-PDF file, and a corrupt or truncated file all fail with
      a single clear error and return **no partial pages**.
- [ ] A PDF encrypted with an empty user password is readable without asking for
      a password.
- [ ] A PDF that requires a real password is rejected rather than half-parsed.

## Text quality

- [ ] Lowercase, then tokenise with `[a-z]+|\d+` when comparing against gold
      evidence; keep digits, they carry the financial facts.
- [ ] Compare the parsed page against an independent extraction of the same page
      and expect a token-multiset F1 near 1.0 on evidence pages. A materially
      lower value means the extractor changed or the page was truncated.
- [ ] Per-page token counts should sit within a narrow band of the reference
      extraction (roughly 90%–110%); outside that band, look for dropped tables
      or duplicated headers.

## Projection

- [ ] The indexed text is the document id, a newline, then the page text — the
      id line doubles as the document-scoping token.
- [ ] Metadata carries exactly the document id, the digest, and the physical page
      number. Financial reports print their own page numbers; never substitute
      those for the physical page.