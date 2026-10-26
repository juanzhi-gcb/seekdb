#!/usr/bin/env python3
"""Audit a filing PDF before indexing it page by page.

Reports the physical page count, which pages carry no extractable text, and any
page whose indexed text would exceed a typical memory-entry byte limit. Run this
before loading a corpus: one oversized page is enough to abort ingestion.

Usage:
    python check_pdf_pages.py REPORT.pdf [--entry-limit 8192] [--document-id ID]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def audit(path: Path, document_id: str, entry_limit: int) -> int:
    try:
        from pypdf import PdfReader
    except ImportError:  # pragma: no cover - dependency hint
        print("pypdf is required: pip install pypdf", file=sys.stderr)
        return 2

    reader = PdfReader(str(path))
    if reader.is_encrypted and not reader.decrypt(""):
        print(f"{path.name}: encrypted with a non-empty password", file=sys.stderr)
        return 1

    blank_pages: list[int] = []
    oversize_pages: list[tuple[int, int]] = []
    largest = 0
    total = len(reader.pages)
    for number, page in enumerate(reader.pages, start=1):
        text = page.extract_text() or ""
        size = len(f"{document_id}\n{text}".encode("utf-8"))
        largest = max(largest, size)
        if not text.strip():
            blank_pages.append(number)
        if size > entry_limit:
            oversize_pages.append((number, size))

    print(
        f"{path.name}: pages={total} blank={len(blank_pages)} "
        f"oversize={len(oversize_pages)} max_page_bytes={largest}"
    )
    if blank_pages:
        preview = ", ".join(str(number) for number in blank_pages[:10])
        print(f"  blank pages: {preview}{' ...' if len(blank_pages) > 10 else ''}")
    for number, size in oversize_pages[:10]:
        print(f"  page {number} exceeds the {entry_limit}-byte entry limit ({size} bytes)")
    return 1 if oversize_pages else 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Audit a filing PDF before page-level indexing.")
    parser.add_argument("pdf", type=Path, help="path to the original PDF")
    parser.add_argument("--document-id", default=None, help="identity prefixed to every indexed page")
    parser.add_argument("--entry-limit", type=int, default=8192, help="byte limit of one indexed entry")
    args = parser.parse_args()
    if not args.pdf.is_file():
        print(f"not a file: {args.pdf}", file=sys.stderr)
        return 2
    document_id = args.document_id or args.pdf.stem
    return audit(args.pdf, document_id, args.entry_limit)


if __name__ == "__main__":
    raise SystemExit(main())