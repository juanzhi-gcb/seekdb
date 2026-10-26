---
name: financebench-pdf-evidence
description: >
  Parse a complete financial filing PDF page by page and retrieve the exact
  evidence page for a natural-language question entirely offline. Use when
  someone needs to index annual reports or 10-K filings, answer
  FinanceBench-style questions and cite the physical page, or debug why a
  page-level retrieval returned the wrong page. Triggers on "10-K", "annual
  report", "FinanceBench", "evidence page", "page-level citation", or
  "why did retrieval miss this page".
---

# FinanceBench PDF evidence retrieval

Turn one filing PDF into citation-ready page evidence, then answer questions by
returning the physical pages that actually contain the evidence — with no model
calls and no network access.

## When to use

- A user wants a 10-K / annual report searchable at page granularity.
- A question must be answered with a citation that points at one physical page.
- A retrieval run returned plausible pages but missed the page a human would
  cite, and the failure needs a systematic diagnosis.

## Workflow

1. **Parse every physical page.** Use `pypdf` and iterate `reader.pages` in
   order. Emit one record per page with `document_id`, the SHA-256 of the whole
   file, the 1-based physical page number, and the extracted text. Blank pages
   stay in the output with empty text; never drop them and never renumber.
2. **Audit the parse before indexing** with `scripts/check_pdf_pages.py`.
   It reports page count, blank pages and any page whose text exceeds the
   8192-byte entry limit that most memory stores enforce.
3. **Index each page as one unit.** Prefix the indexed text with the document
   id on its own line — it doubles as the cheapest document-scoping token.
4. **Retrieve with short probes, not the raw question.** Long questions are
   admitted too sparsely by full-text search. Issue two-term probes, single-term
   probes for the rarest terms, and one probe per leading stem.
5. **Fuse the probe rankings** rather than trusting any single probe:
   reciprocal-rank fusion over probe ranks, combined with a BM25 score over the
   union of candidates.
6. **Apply two domain priors** before selecting the final page list:
   a statement prior (metrics named in the question point at the income
   statement, balance sheet, or cash-flow statement) and a document prior (when
   a scope holds several filings, restrict to the company the question names).
7. **Return distinct pages only**, each carrying an exact citation, and stop at
   the caller's limit.

## Domain priors worth encoding

| Question mentions | Lift pages whose heading contains |
| --- | --- |
| margin, revenue, sales, profit, tax, operating, COGS, inventory | statements of income / operations / earnings |
| liability, asset, balance, debt, liquidity, payable, inventory, capital | balance sheets / statement of financial position |
| cash, capex, dividend, cash flow, depreciation | statements of cash flows |
| segment, division | business segment data / segment information |

The priors are hints, not filters. Keep them additive so an unusual filing does
not lose every page.

## Pitfalls that cost real score

- Splitting on a fixed byte count cuts multi-byte characters and table cells.
  Split on physical pages; within a page keep the text whole.
- A page's first 300 characters carry its heading. Weight terms found there far
  above the same terms buried in a table.
- Adjacent-page spreading and blind neighbour filling both hurt: the evidence
  page is rarely the neighbour, and the slots it steals were paying for
  themselves.
- Summing term weights over a `set` makes ranking depend on the interpreter's
  hash seed. Iterate terms in sorted order so the same question always returns
  the same pages.
- A page whose text exceeds the memory entry limit will abort ingestion. Detect
  it during the audit, before the corpus is loaded.

## Example

```bash
python scripts/check_pdf_pages.py PEPSICO_2022_10K.pdf
# pages=503  blank=0  oversize=0  max_page_bytes=4951

python scripts/check_pdf_pages.py AMERICANWATERWORKS_2021_10K.pdf
# pages=181  blank=4  oversize=1  max_page_bytes=8625
#   blank pages: 151, 153, 155, 157
#   page 144 exceeds the 8192-byte entry limit (8625 bytes)
```

The second case is the one that silently breaks an ingestion pipeline; the audit
catches it in a second instead of after a failed run.

## Reference

- `references/retrieval-playbook.md` — the full probe, fusion and prior recipe
  with the parameters that were measured.
- `references/parsing-checklist.md` — review list for page identity, revisions
  and malformed input handling.
<!-- OB-S:9uNSTpwajT5X8zDiHS7SCg -->
