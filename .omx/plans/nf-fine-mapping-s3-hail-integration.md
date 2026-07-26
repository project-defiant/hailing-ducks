# Plan: S3 Hail Reads for nf-fine-mapping

## Requirements Summary

Use `hailing-ducks` from `nf-fine-mapping` so LD BlockMatrix (`.bm`) and HailTable (`.ht`) inputs can be read directly from object storage, starting with S3. The immediate question is feasibility: prove direct S3 reads for both `hail_scan_blockmatrix(path)` and `hail_scan_table(path)` before changing the pipeline contract.

Current evidence:
- `hail_scan_blockmatrix` opens metadata and partition files through DuckDB `FileSystem`/`FileHandle`, which is intended to support local, S3, GCS, and HTTP paths (`src/hail_blockmatrix_scanner.cpp:40`, `src/hail_blockmatrix_scanner.cpp:193`, `src/hail_blockmatrix_scanner.cpp:299`).
- `hail_scan_table` opens gzipped metadata and row partitions through the same VFS path (`src/hail_table_scanner.cpp:35`, `src/hail_table_scanner.cpp:311`, `src/hail_table_scanner.cpp:405`).
- The existing raw scanners are sequential readers. The new LD query surface must be described as batch-optimized: HT resolution batches requested variants by locus and prunes row partitions; BM extraction batches requested pairs by block and caches decompressed blocks.
- BlockMatrix already has an HTTP/VFS integration test (`test/sql/hail_blockmatrix_http.test:1`) and an explicit `s3://` unknown-scheme test (`test/sql/hail_blockmatrix.test:45`), but no live S3 test.
- `nf-fine-mapping` already models LD as `hail_table` + `block_matrix` inputs to `SubsetLD` (`modules/ld/main.nf:5`, `modules/ld/main.nf:12`).
- Existing `nf-fine-mapping` LD reference data uses cloud Hail paths in `testdata/ld_reference.tsv:1`, while PanUKBB prep still assumes local downloaded `.ht` directories and Java/Hail/Gentropy (`scripts/reference_data/prepare_panukbb_ld_reference.sh:52`, `scripts/reference_data/prepare_panukbb_ld_reference.sh:66`).

## Acceptance Criteria

- A DuckDB SQL smoke test can `LOAD httpfs`, load `quack`, configure S3, and run a bounded query against an S3 `.ht` using `hail_scan_table`.
- A bounded query against an S3 `.bm` using `hail_scan_blockmatrix` succeeds or produces a specific actionable gap.
- `hail_scan_table` avoids decoding unused top-level fields when the DuckDB query projects a subset of columns.
- `query_ld_matrix_events` is documented and tested as a batch-optimized HT/BM query path, distinct from the raw full-scan readers.
- Variant index resolution can prune HailTable row partitions for supported locus predicates using `_jRangeBounds`, then apply exact allele/variant matching inside the selected partitions.
- Trailing-slash and no-trailing-slash S3 paths behave the same for both scanners.
- The result is documented with exact bucket path, DuckDB settings, query, elapsed time, and failure mode if any.
- `nf-fine-mapping` has a proposed pipeline contract for passing remote LD references without staging whole Hail directories locally.

## Implementation Slices

### 1. Live S3 Smoke Harness in hailing-ducks

Add an opt-in script or SQLLogicTest target, e.g. `make test_s3`, gated by env vars:

- `HAILING_DUCKS_S3_HT`
- `HAILING_DUCKS_S3_BM`
- optional `AWS_REGION`, `AWS_NO_SIGN_REQUEST`, `AWS_PROFILE`

The script should load `httpfs`, load the local extension, run `SELECT COUNT(*)` or `LIMIT 1` style queries for `.ht`, and a highly bounded BlockMatrix query. It must skip clearly when env vars or credentials are absent.

### 2. Fix Scanner Path/Remote Edge Cases

Use the harness result to patch only proven problems. Likely candidates:

- Normalize root paths to avoid `s3://...ht//metadata.json` when callers pass trailing slashes.
- Replace any remote-hostile file-size assumptions if DuckDB `httpfs` cannot provide `GetFileSize` for specific S3 objects.
- Ensure S3 errors are user-facing DuckDB errors, not raw low-level exceptions.

### 3. Define nf-fine-mapping LD Contract

Add or update `params.ld_reference` handling so rows can contain remote `s3://...bm` and `s3://...ht` values without Nextflow staging them as `path(...)`. The current `SubsetLD` process already accepts `hail_table` and `block_matrix` as `val(...)`, which is good; keep remote URIs as strings.

### 4. Decide Runtime Surface for subset_ld

Choose where `subset_ld` lives:

- Preferred: a small DuckDB-backed CLI in the collector container that loads `quack` and queries remote Hail data directly.
- Alternative: keep `subset_ld` as a separate binary/container, but make its only hard contract `--hail-table URI --block-matrix URI --variants parquet --output bgzip`.

The CLI must install/load DuckDB `httpfs` and `quack`, configure S3 credentials, read the variant intersection parquet, map variants through the HailTable index, then fetch only required LD matrix entries.

The DuckDB-facing user experience should support a fine-mapping locus table with an explicit requested-variant list:

```sql
SELECT f.*, ld.*
FROM fine_mapping_locus f
CROSS JOIN LATERAL query_ld_matrix(
  's3://.../UKBB.AFR.ldadj.variant.b38.ht/',
  's3://.../UKBB.AFR.ldadj.bm',
  f.locus,
  f.variantIds
) ld;
```

Use `query_ld_matrix(ht_path, bm_path, locus, variant_ids, ...)` as the canonical SQL surface. The production/materializer request input should keep one row per fine-mapping locus with a list of requested variants:

```text
locus_id, locus, variant_ids
```

The locus interval is the storage-pruning envelope; `variant_ids` is the exact requested output selector. Preserve the locus object shape used by nf-fine-mapping and avoid expanding the HT lookup into per-variant queries.

Do not implement HT resolution as one lookup per requested variant. For each locus request, parse the `variant_ids` list once and build compact in-memory sets:

```text
direct_set: requested variant ids
flipped_set: same variants with ref/alt swapped
```

Before scanning HT, validate each requested variant's parsed contig/position against the supplied locus interval. Variants outside the locus must be reported with `outside_locus` status and excluded from the direct/flipped sets for that locus. Then scan each HT partition selected by the locus range once. For each decoded HT row, build the HT variant id and check membership in `direct_set` and `flipped_set`. This is equivalent to a batched `list_overlap` plan and avoids per-variant HT queries. Status output can still be variant-shaped, but it is produced from the set-hit map after the batched scan.

The implementation should still plan work in batches internally. Per-row lateral execution is acceptable for a small interactive query, but nf-fine-mapping multi-locus execution should materialize the locus requests first and run a batch planner so metadata reads and BlockMatrix partition reads can be shared across loci. The physical BlockMatrix reader must cache decompressed blocks within a query execution so repeated or overlapping loci do not re-fetch the same object from S3.

Precise optimization claim:

- `hail_scan_table(path)` and `hail_scan_blockmatrix(path)` remain general raw readers.
- `query_ld_matrix_events(...)` is the batch-optimized LD reader: it batches HT lookups by locus/variant list, prunes HT partitions, batches BM cell requests by block, and caches decompressed BM blocks.
- Any README or issue wording should say "LD queries are batch-optimized" rather than "all HT/BM scans are predicate-pushed."

Avoid a user-facing `query_id` lifecycle in the first implementation. It avoids duplicate IO but forces users to understand temporary query state and call functions in the right order. Instead:

- Production path: provide a CLI/materializer that performs one physical extraction and writes both outputs:

```text
ld_pairs.parquet
variant_resolution_status.parquet
```

- DuckDB implementation path: use one compact internal event stream, materialize it once, then split it into pair and status outputs.

```sql
CREATE TEMP TABLE _ld_events AS
SELECT *
FROM query_ld_matrix_events(ht_path, bm_path, requests_path);

COPY (
  SELECT locus_id, variant_id_i, variant_id_j, idx_i, idx_j, r
  FROM _ld_events
  WHERE event_type = 0
) TO 'ld_pairs.parquet' (FORMAT parquet);

COPY (
  SELECT locus_id, requested_variant_id, matched_variant_id, idx, allele_order, ht_status
  FROM _ld_events
  WHERE event_type = 1
) TO 'variant_resolution_status.parquet' (FORMAT parquet);
```

`event_type` values:

- `0`: LD pair row.
- `1`: variant status row.
- `2`: optional BM pair status row, emitted only when requested for debugging or strict audit.

Direct SQL users can query `query_ld_matrix(...)` for pairs only, or `query_ld_matrix_events(...)` when they want pair rows and statuses from one physical pass. The nf-fine-mapping path should call the materializer/CLI rather than asking users to manage temp tables manually.

Locked DuckDB/materializer contract:

- Production input is a request file path, not a relation-valued argument:

```text
query_ld_matrix_events(ht_path, bm_path, requests_path)
```

- `requests_path` is Parquet with:

```text
locus_id VARCHAR
locus VARCHAR
variant_ids LIST<VARCHAR>
```

- `query_ld_matrix_events` emits a compact event stream:

```text
event_type TINYINT
locus_id VARCHAR
variant_id_i VARCHAR
variant_id_j VARCHAR
requested_variant_id VARCHAR
matched_variant_id VARCHAR
idx_i BIGINT
idx_j BIGINT
idx BIGINT
allele_order TINYINT
r DOUBLE
ht_status TINYINT
bm_status TINYINT
```

Fields are nullable when they do not apply to the row's `event_type`.

- Variant status rows are emitted for every unique requested variant after exact deduplication.
- BM status rows are emitted only for failures by default; successful BM pairs are represented by LD pair rows.
- Query-local decompressed block cache is required. First implementation should expose `max_cached_blocks`, defaulting to `64`.
- User-facing outputs are deterministic: LD pairs ordered by `locus_id, idx_i, idx_j`; variant statuses ordered by `locus_id, requested_variant_id`.
- Hard errors stop the query only for structural/runtime failures: invalid request schema, unreadable metadata, corrupt block data, or unsupported BM metadata.

### 5. HailTable Pushdown and Batch Variant Resolution

Expose two levels of optimization:

- Generic `hail_scan_table` projection pushdown: when a query only selects fields like `locus`, `alleles`, and `idx`, initialize the scanner with those projected column ids and skip unprojected fields on the wire.
- Purpose-built variant index resolution: add a function or CLI path that accepts locus rows with `variant_ids` lists, reads `_jRangeBounds` from `rows/metadata.json.gz`, selects only partitions whose row-key bounds overlap the locus intervals, builds per-locus direct/flipped variant-id sets, decodes minimal HT fields once per selected row, and records set hits as `variant_id -> idx` matches.

Do not promise arbitrary SQL predicate pushdown in the first implementation. DuckDB may pass filters to a table function, but only predicates that can be mapped to Hail row bounds reduce S3 partition IO. Other predicates should remain normal DuckDB filters after scan.

Variant matching policy:

- Treat liftover as out of scope for hailing-ducks. The HT provided to nf-fine-mapping is expected to contain the coordinate system the pipeline wants to query, e.g. pre-prepared hg38 variant rows.
- Do not normalize contig naming or allele representation for users. Requested `variant_ids` and `locus` contigs must use the same convention as the provided HT, e.g. `1` vs `chr1`. Mismatches should surface as `not_found_in_ht` or `outside_locus`, not silent correction.
- Do not support multi-allelic variants. Requested `variant_ids` must parse as biallelic `contig_position_ref_alt`. HT rows whose `alleles` array is not length 2 are not eligible for matching and should produce `unsupported_variant_id` or `ambiguous_in_ht` for affected requested variants rather than being coerced.
- For every locus, build both the exact requested variant-id set and the flipped allele variant-id set before scanning HT rows.
- If a requested variant parses successfully but its contig/position is outside the supplied locus interval, emit `outside_locus` and do not attempt HT or BM resolution for that variant.
- During the pruned HT scan, a row whose HT-derived variant id is in the exact set records a direct hit; a row whose HT-derived variant id is in the flipped set records a flipped hit for the original requested variant.
- A direct match where HT alleles are `[ref, alt]` emits `matched_variant_id = requested_variant_id` and `allele_order = 1`; a flipped match where HT alleles are `[alt, ref]` emits `matched_variant_id = flipped_variant_id` and `allele_order = -1`.
- Prefer direct orientation if both direct and reversed records resolve to the same `idx`, mirroring Gentropy's PanUKBB aligned-index behavior.
- Do not hardcode PanUKBB liftover flags (`locus_fail_liftover`, `ref_allele_mismatch`, `ref_alt_flip`) into the generic resolver. If those fields exist, expose them as optional diagnostics. The caller or prepared HT decides whether they are meaningful.
- Emit unresolved and ambiguous variants explicitly instead of silently dropping them.

The resolved index rows should carry orientation through to BlockMatrix extraction:

```text
locus_id, requested_variant_id, matched_variant_id, idx, allele_order, ht_status
```

Use compact integer status codes in machine-facing output. Keep the code table in documentation and optionally expose a tiny status lookup table for interactive inspection.

Per-variant HT status codes:

- `0`: `resolved_exact` - requested `variantId` was present in HT.
- `1`: `resolved_flipped` - requested `variantId` was absent but the flipped allele variant id was present in HT.
- `2`: `not_found_in_ht` - neither requested nor flipped variant id was present in HT within the locus envelope.
- `3`: `outside_locus` - requested variant coordinates are outside the supplied locus.
- `4`: `ambiguous_in_ht` - multiple HT rows match and cannot be ranked safely.
- `5`: `unsupported_variant_id` - variant id cannot be parsed into `contig_position_ref_alt` or has an unsupported allele shape.
- `6`: `multiple_variants_at_position` - the same locus request contains distinct variant IDs at the same contig/position.

BlockMatrix LD output should apply sign correction as:

```text
r_out = r_matrix * allele_order_i * allele_order_j
```

Diagonal rows are not emitted.

Per-pair BM status codes:

- `0`: `resolved` - both variants resolved in HT and the requested matrix value was read from BM.
- `1`: `missing_from_ht` - at least one requested variant did not resolve in HT, so no BM lookup was attempted.
- `2`: `idx_out_of_bounds` - HT emitted an `idx` outside BM dimensions.
- `3`: `missing_block` - the required BM block is absent from metadata/filtered block files.
- `4`: `missing_or_nan_value` - the BM block was present but the decoded value is not usable.

Expose a tiny lookup table for interactive use:

```sql
SELECT *
FROM hail_ld_status_codes();
```

Output:

```text
status_domain, status_code, status_name, description
```

`status_domain` is `ht` or `bm`.

TDD fixtures:

- Multi-part HailTable fixture with `_jRangeBounds` covering disjoint contig/position ranges.
- Test proving a locus interval query returns the same rows as a full scan plus filter.
- Test proving the pruned resolver reads only the expected partition files, using a local fixture path where opening an unexpected partition fails.
- Test proving direct and reversed allele matches resolve with `allele_order` values of `1` and `-1`.
- Test proving sign correction is applied once when BlockMatrix pairs are emitted.
- Test proving a requested variant absent from HT but present as a flipped allele is reported as `resolved_flipped`.
- Test proving completely absent variants are reported as `not_found_in_ht` and excluded from BM lookup.
- Test proving HT-resolved variants whose BM block/value is unavailable are reported separately from HT misses.
- Test proving exact duplicate requested variant IDs are deduplicated, while distinct requested variants at the same contig/position are reported as `multiple_variants_at_position` and excluded from LD output.

### 6. BlockMatrix Batch Planner

After HT resolution emits matrix `idx` values, group work by BlockMatrix block id:

1. Map each requested matrix index to `floor(idx / blockSize)`.
2. Build unique block-pair ids needed for each locus/window.
3. Read each `.bm` partition once.
4. Emit all requested in-block LD cells from that decompressed block.

Support two query modes, ordered by implementation priority:

- Multiple variants within one locus: canonical all-vs-all pairs for the resolved `idx` values in that locus/window. Group by block pair and read each touched block once.
- Multiple loci: independent locus/window batches in one call. Preserve `locus_id` in the output and de-duplicate block reads across loci when windows overlap.

The first production scope is canonical within-locus/window LD pairs. Avoid arbitrary global cross-locus pair queries until there is a concrete consumer.

Default output should be one row per canonical pair:

```text
locus_id, variant_id_i, variant_id_j, idx_i, idx_j, r
```

Use strict canonical `idx_i < idx_j` ordering. Diagonal rows are not emitted and no diagonal flag is needed in the first implementation.

#### Use Case 1: One Locus

Goal: produce an LD table for the exact requested variants inside a locus/window.

Input:

```text
locus_id, locus, variant_ids
```

Planning:

- Parse `locus` as `contig:start-end`.
- Preflight `variant_ids`: exact duplicate variant IDs in the same locus are deduplicated; distinct variant IDs at the same contig/position in the same locus are retained in status output as `multiple_variants_at_position` and excluded from HT/BM matching.
- Use the locus interval to prune HT partitions and BlockMatrix block ranges.
- Resolve only requested `variant_ids` against HT rows inside the locus interval.
- Preserve unresolved requested variants in the status output.
- Sort or group by `idx` for deterministic output.
- Generate canonical pairs among resolved requested variants with `idx_i < idx_j`.
- Group pairs by block pair and read each touched BlockMatrix partition once.
- Canonicalize every matrix lookup to `(min(idx_i, idx_j), max(idx_i, idx_j))`. The committed `test/matrix.bm` fixture decodes as upper-triangular: sampled upper cells are nonzero, sampled lower cells are zero, and diagonal cells are approximately 1.0. A PanUKBB S3 smoke test must confirm the same storage orientation across block files before relying on it in production.
- Apply `r_out = r_matrix * allele_order_i * allele_order_j`.

Output:

```text
locus_id, variant_id_i, variant_id_j, idx_i, idx_j, r
```

Failure behavior:

- If fewer than two requested variants resolve, emit no LD rows for the locus and report `insufficient_resolved_variants`.
- If some requested variants are not found, ambiguous, or have unsupported allele shape, continue with resolved variants and include a sidecar status table.
- Do not enforce a minimum resolved fraction in hailing-ducks. The extension should be mechanical: resolve what exists, emit canonical LD pairs when at least two variants resolve, and leave biological QC thresholds to nf-fine-mapping.

#### Use Case 2: Multiple Loci

Goal: query many locus/window batches in one run while keeping outputs attributable to their source locus.

Input:

```text
locus_id, locus, variant_ids
```

Planning:

- Parse and resolve each locus independently.
- Build pair plans independently per `locus_id`.
- Merge the physical block-read plan across loci so overlapping loci do not re-read the same BlockMatrix partition.
- Maintain a per-query block cache keyed by `block_idx` or partition path. The cache should store the decompressed block payload plus block metadata, not individual LD cells.
- Bound cache size with an explicit setting or named parameter; when the cache is full, evict least-recently-used blocks. The first implementation can default to query-local unbounded caching for small tests, then add limits before production S3 use.
- Keep a per-block list of requested output cells keyed by `locus_id`.
- Emit rows grouped deterministically by `locus_id`, then `idx_i`, then `idx_j`.

Output:

```text
locus_id, variant_id_i, variant_id_j, idx_i, idx_j, r
```

Failure behavior:

- Treat each locus independently for missingness and thresholds.
- A failed or underspecified locus should not fail unrelated loci unless the CLI is configured with `--on-missing fail`.
- Do not enforce a global or per-locus minimum resolved fraction in hailing-ducks.

### 7. Pipeline-Level Smoke

Add an opt-in Nextflow/nf-test profile for cloud LD smoke tests. It should run one tiny locus against public or configured S3 LD references and assert an LD output file is produced with non-empty expected pairs. Keep full production-scale validation separate.

## Risks and Mitigations

- **Public S3 auth/region mismatch**: make region and anonymous/requester-pays settings explicit in the smoke harness.
- **Remote reads are too chatty for large BlockMatrix scans**: resolve HT indices in batches, prune HT partitions by row bounds, then read each required BlockMatrix partition once.
- **Repeated block fetches across overlapping loci**: keep a query-local decompressed block cache and merge requested cells by block before reading.
- **Generic SQL filter pushdown over-promises IO savings**: only treat row-key/locus filters as storage-prunable; keep all other predicates as normal post-scan filters.
- **Nextflow stages URI strings accidentally as files**: keep `.ht`/`.bm` as `val`, not `path`, in process contracts.
- **Container distribution friction**: test extension loading inside the collector/subset container before changing the main workflow.

## Verification Steps

1. `make test_s3` with a known public S3 HailTable and BlockMatrix path.
2. Local regression: `make test`, `make test_http`, `make format-check`.
3. `nf-test test --stub` remains green in `nf-fine-mapping`.
4. Opt-in cloud smoke profile runs one minimal LD extraction and writes a non-empty output.

## Grilling Questions

1. What exact S3 source should be the first proof target: PanUKBB LD release, gnomAD LD mirrored to S3, or another bucket?
2. Should the first integration target be `hailing-ducks` only, or should it immediately include an `nf-fine-mapping` smoke profile?
3. Is the desired production path public anonymous S3, authenticated S3, requester-pays, or all three?
4. Can the requested variant input be required to include normalized `contig`, `position`, `ref`, and `alt`, grouped by locus/window? This is what makes HailTable partition pruning and BlockMatrix block batching effective.
5. For BlockMatrix output, should diagonal rows be emitted by default, or only when explicitly requested?
