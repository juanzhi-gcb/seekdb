# Retrieval playbook

The parameters below were measured on real filings, not chosen by taste. Treat
them as a starting point and re-measure whenever the corpus changes.

## 1. Probe generation

Long natural-language questions are a bad query for page-level full-text
search: the engine admits too few documents. Decompose instead.

| Probe kind | Example | Fusion weight |
| --- | --- | ---: |
| two-term pair over the leading terms | `operating income` | 1.0 |
| single rare term | `ebitda` | 0.5 |
| the whole question, untouched | `...` | 1.0 |
| synonym or stem of a leading term | `litigation` | 0.3 |

Rules of thumb:

- Cap the leading terms at about eight; beyond that the pairs only add noise.
- Expand the first few terms with filing vocabulary, not with generic
  synonyms: `geographies → countries, regions, segments`; `legal →
  litigation, proceedings, claims`; `capital → expenditures, property`.
- Never issue a synonym as a strict AND partner of the term it is meant to
  broaden — the page usually contains one spelling, not both.
- Skip instruction words that every benchmark question carries
  (`state`, `explain`, `metric`, `assume`, `perspective`); they are pure noise
  in the index.

## 2. Fusion

Reciprocal-rank fusion across probes, combined with lexical scoring:

```
score = lexical_norm + 0.6 * rrf_norm
rrf   = sum over probes of weight / (60 + rank_in_probe)
```

`lexical_norm` is a BM25 score over the union of candidates, normalised by the
best candidate; `rrf_norm` is the fused contribution normalised the same way.
Normalising both before adding keeps the two scales from fighting.

BM25 defaults that worked: `k1 = 1.2`, `b = 0.75`. Lowering `b` to 0.3 changed
nothing on filings whose pages are of similar length.

## 3. Priors

Two priors earn their place; both are additive, never filters.

**Heading prior.** The first ~300 characters of a page carry its section
title. A query term found there is worth more than the same term deep inside a
table. Measured contribution: `+0.8 * idf(term)` per heading term.

**Statement prior.** Map the metrics a question names onto the statement that
reports them, and lift pages whose heading matches:

- income statement — margin, revenue, sales, profit, tax, operating, COGS
- balance sheet — liability, asset, debt, liquidity, payable, inventory, capital
- cash-flow statement — cash, capex, dividend, depreciation
- segment tables — segment, division

A boost of `0.8 × peak lexical score` moved the right statement into the top
20 for several filings and never hurt the others.

## 4. Multi-document scopes

When one scope holds several filings, generic terms make every company compete
for the same slots. Two steps fix most of it:

1. **Detect the document from the question.** Compare the compacted question
   against each document's own id prefix (`AMERICANEXPRESS` matches
   "American Express's"). If exactly one document matches, use it.
2. **Scope the probes.** Issue `<document-term> <query-term>` probes; full-text
   AND semantics then only return pages of that filing. Add a modest final
   multiplier for the detected document's pages.

If no name matches, fall back to the document that owns most of the first-pass
evidence, and only when it holds a clear majority.

## 5. What did not work

Measured and rejected, so they are not worth re-trying blind:

| Attempt | Effect |
| --- | --- |
| pseudo-relevance feedback (RM3-style) | no change |
| instruction-word filtering | no change |
| probe limit 100 → 200 | recall dropped |
| neighbour score spreading (window 1 or 2) | recall dropped |
| reserving slots for neighbouring pages | recall dropped badly |
| synonym weight 0.3 → 0.45 or 0.5 | no change |
| restricting the ranking to the named document | no change on the weak set |
| stemming the index vocabulary | recall dropped |

## 6. Diagnose before tuning

For every miss, record three facts before changing anything:

1. Is the gold page in the candidate pool at all?
2. If yes, what rank does it hold?
3. Which of the question's terms does the page actually contain?

A page absent from the pool needs broader probes or a better prior; a page at
rank 22 needs scoring work. Confusing the two wastes iterations.