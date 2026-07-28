-- Batch-optimized LD query pipeline walkthrough, using the committed synthetic fixtures
-- (test/hailtable_fixture_ld.ht + test/matrix_ld.bm), so it runs with no external data or network
-- access. For real PanUKBB S3 data, see README.md's "Real S3 data" section / `make test_s3_smoke`.
--
-- Run from the repo root, after `make release`:
--   ./build/release/duckdb < examples/ld_query_walkthrough.sql
--
-- Full schema/status-code/policy reference: docs/LD-QUERY.md

.echo on

-- Step 1: build a request file. One row per fine-mapping locus: locus_id, a "contig:start-end"
-- locus interval (used to prune HailTable partitions and to flag out-of-locus variants), and the
-- variant IDs you want LD for, as "contig_position_ref_alt".
COPY (
  SELECT * FROM (VALUES
    ('locus1', 'chr1:100-500', ['chr1_200_A_G', 'chr1_300_A_G']),
    ('locus2', 'chr1:600-1000', ['chr1_650_A_T'])
  ) AS t(locus_id, locus, variant_ids)
) TO '/tmp/hailing_ducks_example_requests.parquet' (FORMAT PARQUET);

-- Step 2 (optional, for interactive inspection): preflight validates request-file structure only
-- (dedup, outside-locus, conflicting same-position variants) -- no HailTable/BlockMatrix access yet.
SELECT * FROM hail_ld_preflight_requests('/tmp/hailing_ducks_example_requests.parquet')
ORDER BY locus_id, requested_variant_id;

-- Step 3 (optional, for interactive inspection): resolve requested variants against the real
-- HailTable, using locus-pruned partition scanning and direct + flipped allele-id matching.
-- chr1_300_A_G resolves "flipped" (allele_order = -1) because the fixture's HT row stores it as
-- G/A; chr1_650_A_T has no matching HT row at all (not_found_in_ht, status_code = 2).
SELECT * FROM hail_ld_resolve_ht('test/hailtable_fixture_ld.ht', '/tmp/hailing_ducks_example_requests.parquet')
ORDER BY locus_id, requested_variant_id;

-- Step 4: the production path. One physical HT+BM pass writes both user-facing outputs, streamed
-- straight to Parquet so peak memory stays bounded regardless of how many loci/pairs the request
-- contains (verified against a real 24-locus/38.5M-pair combined PanUKBB request).
SELECT * FROM hail_ld_materialize(
  'test/hailtable_fixture_ld.ht',
  'test/matrix_ld.bm',
  '/tmp/hailing_ducks_example_requests.parquet',
  '/tmp/hailing_ducks_example_ld_pairs.parquet',
  '/tmp/hailing_ducks_example_variant_status.parquet'
);

-- ld_pairs.parquet: successful pairs only (locus2's chr1_650_A_T never resolved, so it contributes
-- no pair -- only locus1's one resolvable pair, chr1_200_A_G x chr1_300_A_G, appears here).
SELECT * FROM '/tmp/hailing_ducks_example_ld_pairs.parquet' ORDER BY locus_id, idx_i, idx_j;

-- variant_resolution_status.parquet: one row per unique requested variant, including failures --
-- the QC-facing companion output, decoupled from the LD pairs themselves.
SELECT * FROM '/tmp/hailing_ducks_example_variant_status.parquet' ORDER BY locus_id, requested_variant_id;

-- The lookup table for every status_code above.
SELECT * FROM hail_ld_status_codes() ORDER BY status_domain, status_code;
