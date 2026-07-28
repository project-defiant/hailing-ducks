# Batch-optimized LD query

Reference documentation for `hailing-ducks`' batch-optimized LD query surface, implemented per
GitHub PRD [#23](https://github.com/project-defiant/hailing-ducks/issues/23) (issues #17-#22).

## Raw readers vs. the batch-optimized LD query path

`hail_scan_table(path)` and `hail_scan_blockmatrix(path)` (documented in the top-level `CLAUDE.md`)
are **raw, sequential readers**: they decode a whole `.ht`/`.bm` file's rows/blocks as-is, with no
predicate pushdown, no locus pruning, and no cross-file (HT+BM) logic. They're the right tool for
inspecting or converting a Hail dataset directly.

The functions on this page are a **separate, purpose-built path** for one specific job: given a
fine-mapping locus (a genomic interval plus a list of requested variant IDs), resolve exactly those
variants against a real HailTable and extract exactly the LD pairs among them from a real
BlockMatrix — without scanning either file in full, and without requiring Hail, Spark, or a JVM. Do
not expect generic `WHERE`-clause pushdown on `hail_scan_table`/`hail_scan_blockmatrix` themselves;
that's explicitly out of scope (see PRD "Out of Scope").

## Pipeline stages

```
request file (Parquet)              real HailTable (.ht)         real BlockMatrix (.bm)
  locus_id, locus,                        │                              │
  variant_ids LIST<VARCHAR>               │                              │
        │                                 │                              │
        ▼                                 ▼                              │
  hail_ld_preflight_requests  ──────────────────▶ hail_ld_resolve_ht      │
  (structural validation only,      (partition-pruned HT resolution:      │
   no HT/BM touched yet)             direct + flipped allele matching)    │
        │                                       │                        │
        │                            resolved_exact/resolved_flipped     │
        │                            (idx, allele_order) only            │
        │                                       ▼                        ▼
        │                            hail_ld_bm_pairs_batch (multi-locus, shared block cache)
        │                                       │
        └───────────────────────────────────────┴──────────────▶ hail_ld_materialize
                                                                  (one physical HT+BM pass,
                                                                   writes both output files)
```

- **`hail_ld_preflight(request_arg)`** / **`hail_ld_preflight_requests(requests_path)`** — structural
  validation only (parsing, dedup, outside-locus, unsupported-format, same-position-conflict
  detection). Never touches an HT or BM. `hail_ld_preflight` takes a single hand-rolled
  `'locus_id|contig:start-end|var1,var2,...'` string (a TDD/interactive convenience);
  `hail_ld_preflight_requests` takes a real request-file path.
- **`hail_ld_resolve_ht(ht_path, requests_path)`** — the batch HailTable resolver (issue #18).
  Reuses preflight's classification internally, then resolves every structurally-clean candidate
  against the real HT using per-locus direct+flipped allele-ID sets and `_jRangeBounds`-based
  partition pruning.
- **`hail_ld_bm_pairs(bm_path, resolved)`** / **`hail_ld_bm_pairs_batch(bm_path, resolved,
  max_cached_blocks := 64)`** — BlockMatrix pair extraction (issues #19-#20). Takes already-resolved
  `(idx, allele_order)` pairs (as produced by `hail_ld_resolve_ht`, filtered to `status_code IN (0,
  1)`), generates strict canonical pairs, and extracts real LD values with a shared, capacity-bounded
  block cache across loci in the `_batch` variant.
- **`hail_ld_materialize(ht_path, bm_path, requests_path, ld_output_path, status_output_path,
  max_cached_blocks := 64)`** — the materializer/CLI surface (issue #21). Performs the HT resolution
  and BM extraction exactly once each and writes both user-facing outputs from that single pass.
- **`hail_ld_ht_partitions_for_locus(ht_path, locus_range)`** / **`hail_ld_bm_pairs_batch_stats(...)`**
  — introspection helpers proving partition-pruning and block-cache-reuse behavior through
  observable SQL output rather than internal state. Not part of the production pipeline.
- **`hail_ld_status_codes()`** — the status-code lookup table (see below).

> **Composition note:** table function arguments can't contain subqueries. To chain
> `hail_ld_resolve_ht`'s output into `hail_ld_bm_pairs`/`hail_ld_bm_pairs_batch` interactively, go
> through a session variable:
> ```sql
> SET VARIABLE resolved = (
>   SELECT list({'idx': idx, 'allele_order': allele_order})
>   FROM hail_ld_resolve_ht('ht_path', 'requests_path')
>   WHERE locus_id = 'my_locus' AND status_code IN (0, 1)
> );
> SELECT * FROM hail_ld_bm_pairs('bm_path', getvariable('resolved'));
> ```
> `hail_ld_materialize` does this same composition internally, in-process, without the round trip
> (and without invoking the HT/BM resolution twice) — prefer it for anything beyond interactive
> exploration.

## Request file schema

One row per fine-mapping locus:

| Column | Type | Notes |
|---|---|---|
| `locus_id` | `VARCHAR` | Caller-assigned identifier, unique per request row. |
| `locus` | `VARCHAR` | `<contig>:<start>-<end>`, e.g. `chr1:36098798-36652278`. Must match the HT's own contig convention exactly (see "No normalization" below) — PanUKBB b38 tables use `chr1`, not `1`. |
| `variant_ids` | `LIST<VARCHAR>` | Biallelic IDs, `<contig>_<position>_<ref>_<alt>`, e.g. `chr1_36098874_C_T`. |

## Event/output schemas

`hail_ld_resolve_ht` / `hail_ld_preflight_requests`:

| Column | Type | Notes |
|---|---|---|
| `locus_id` | `VARCHAR` | |
| `requested_variant_id` | `VARCHAR` | As given in the request, after exact-duplicate dedup. |
| `matched_variant_id` | `VARCHAR` (nullable) | The HT's own `contig_position_ref_alt`; differs from `requested_variant_id` on a flip. `NULL` unless `status_code IN (0, 1)`. |
| `idx` | `BIGINT` (nullable) | The HT row's own index (the BM join key). `NULL` unless resolved. |
| `allele_order` | `INTEGER` (nullable) | `1` (exact) or `-1` (flipped). `NULL` unless resolved. |
| `status_domain` | `VARCHAR` | Always `"variant"`. |
| `status_code` | `INTEGER` | See status code table. |

`hail_ld_bm_pairs` / `hail_ld_bm_pairs_batch` (the `_batch` variant adds a leading `locus_id` column):

| Column | Type | Notes |
|---|---|---|
| `locus_id` | `VARCHAR` | `_batch` variant only. |
| `idx_i`, `idx_j` | `BIGINT` | Strict canonical pair, `idx_i < idx_j` always; no diagonal rows. |
| `r` | `DOUBLE` (nullable) | Sign-corrected (`raw_value * allele_order_i * allele_order_j`). `NULL` unless `bm_status_code = 0`. |
| `bm_status_domain` | `VARCHAR` | Always `"bm"`. |
| `bm_status_code` | `INTEGER` | See status code table. |

`hail_ld_materialize` returns one summary row: `(ld_pairs_written BIGINT, status_rows_written
BIGINT)`, and writes:

- **`ld_output_path`** (Parquet): `(locus_id, idx_i, idx_j, r)` — **successful pairs only**
  (`bm_status_code = 0`); BM failures are diagnostic, not LD output, and are never written here.
- **`status_output_path`** (Parquet): the full `hail_ld_resolve_ht` schema above — **one row per
  unique requested variant**, including every failure status, straight from the single HT pass.

## Status codes (`hail_ld_status_codes()`)

| domain | code | name | meaning |
|---|---|---|---|
| variant | 0 | `resolved_exact` | Matched an HT row with the exact requested ref/alt order. |
| variant | 1 | `resolved_flipped` | Matched an HT row with ref/alt swapped relative to the request. |
| variant | 2 | `not_found_in_ht` | No HT row at that contig/position, in either orientation. |
| variant | 3 | `outside_locus` | Variant's own contig/position falls outside the request's `locus` interval. Detected before any HT work. |
| variant | 4 | `ambiguous_in_ht` | The HT itself has more than one row matching this candidate (both orientations, or a genuine HT-side duplicate) — reserved exclusively for ambiguity discovered *during* HT resolution. |
| variant | 5 | `unsupported_variant_id` | Not a supported biallelic `contig_pos_ref_alt` format (e.g. an indel — multi-character ref/alt). |
| variant | 6 | `multiple_variants_at_position` | Two or more *distinct* requested IDs share one contig/position (whether or not they're ref/alt flips of each other) — a request-level conflict, detected before any HT work. |
| bm | 0 | `bm_resolved` | LD value successfully extracted. |
| bm | 1 | `bm_missing_in_ht` | Reserved for a pair where one side never resolved in HT; not producible by this resolver's own inputs, since pairs are only ever generated among already-resolved variants. |
| bm | 2 | `bm_index_out_of_bounds` | `idx` falls outside the BM's own `nRows`/`nCols`. |
| bm | 3 | `bm_missing_block` | The BM block containing this cell has no physical part file (Hail's `maybeFiltered` sparse storage). |
| bm | 4 | `bm_missing_or_nan` | The block exists but the specific cell is `NaN` or otherwise missing. |

## Policies

- **No normalization, ever.** Contigs, positions, and alleles are taken exactly as given — no
  liftover, no left-alignment/trimming, no case folding, no reverse-complementing. Request IDs and
  locus contigs must already match the HT's own convention (`chr1`, not `1`, for PanUKBB b38 tables).
  If you need to align an hg19 HT to hg38 yourself, do it with Hail before pointing this extension at
  it — see the git history around 2026-07-28 for a worked example and a real bug this uncovered
  (`ResolveHTCore` used to crash on a HT row with a `NULL` locus, e.g. an unfiltered failed-liftover
  row; fixed, but dropping such rows before finalizing your table is still the right call).
- **Biallelic only.** Requested IDs must be `contig_pos_ref_alt` with single-character `A/C/G/T`
  bases; HT rows whose `alleles` array isn't length 2 are never eligible for matching (not coerced).
- **Exact duplicates collapse; distinct same-position IDs conflict.** A requested ID repeated
  verbatim within one locus is deduplicated silently. Two *different* IDs at the same contig/position
  — flips of each other or not — both get `multiple_variants_at_position` (6) and never reach HT
  resolution; see the commit history around `hail_ld_preflight_flipped.test` for why this changed
  from an earlier (wrong) design that tried to resolve flips at preflight time from request shape
  alone.
- **No diagonal pairs.** `hail_ld_bm_pairs`/`_batch` never emit `idx_i == idx_j`, even defensively
  against a duplicate `idx` in the resolved input.
- **Strict canonical pairs only.** Always `idx_i < idx_j`; no symmetric duplicate, no diagonal, and
  callers never need to reason about which physical BM triangle is stored (see "BlockMatrix storage
  orientation" below — this extension handles it internally).

## BlockMatrix storage orientation (verified against real data)

Real PanUKBB `.bm` files store the **lower triangle inclusive of the diagonal**
(`block_row >= block_col` at the tile level) — confirmed by sampling ~30 stored block indices across
the full range of a real `s3://pan-ukb-us-east-1/ld_release/UKBB.EUR.ldadj.bm`'s `maybeFiltered`
list, with zero counterexamples. This was the opposite of this codebase's original assumption; see
the git history around commits `2ae8b3c` and `5933127` for the full root-cause story (two distinct
bugs: the tile-selection direction, and — separately — the within-tile array-offset convention,
which is *also* not what naive tile-selection would suggest). Callers of the LD query functions
never need to know this; it's handled internally by `hail_ld_bm_pairs`/`_batch`.

## Running the S3 smoke test

`test/sql/hail_ld_s3_smoke.test` (issue #22) validates this whole pipeline end-to-end against a real,
remote PanUKBB-style HT/BM pair over S3/httpfs. It's opt-in and network-dependent:

```sh
HAILING_DUCKS_S3_HT_PATH=s3://pan-ukb-us-east-1/ld_release/UKBB.EUR.ldadj.variant.b38.ht \
HAILING_DUCKS_S3_BM_PATH=s3://pan-ukb-us-east-1/ld_release/UKBB.EUR.ldadj.bm \
make test_s3_smoke
```

No AWS credentials are needed for the public PanUKBB bucket. The test derives its own request from
real rows read directly out of whichever HT path you configure (rather than hardcoding
population-specific variant IDs), so it works against any PanUKBB-style population's HT/BM pair.
Without both env vars set, this test — and only this test — is skipped cleanly; `make test` and
`make test_http` are unaffected and need no network access.

This smoke test is a lightweight, any-environment-runnable regression check. The authoritative
correctness proof behind the BlockMatrix orientation fixes above is an exact match (correlation =
1.0, zero mismatches at a 1e-6 threshold across 488,566 real pairs) against an independent,
Hail-computed LD reference for a real fine-mapping locus — recorded in the git history and the
GitHub issue #19/#20/#21 follow-up comments, not reproduced as a checked-in test (it depends on
externally-generated reference data not present in this repository).
