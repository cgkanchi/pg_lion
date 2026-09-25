"""Profile scheduling checks; no PostgreSQL server needed."""
import json
from pathlib import Path
import random
import unittest
from unittest.mock import Mock

from audit import expected_configurations
from run import Suite, parse_args
from workloads import (LOW_MEMORY_FETCH, ORDERED_CASES, family_cases, measurement_matrix,
                       profile_cases, profile_indexes)


class Profiles(unittest.TestCase):
    def args(self, *extra):
        return parse_args(['--prefix', '/unused', '--output', '/unused', *extra])

    def test_defaults_and_overrides(self):
        args = self.args()
        self.assertEqual(args.families, ['btree', 'gin', 'roaring', 'roaring_btree'])
        self.assertEqual(args.rows, [1000000, 5000000])
        self.assertEqual((args.repeats, args.build_repeats, args.cold_repeats, args.duration), (3, 1, 0, 0))
        full = self.args('--profile', 'full', '--repeats', '2', '--cold-repeats', '0')
        self.assertIn('brin', full.families)
        self.assertEqual((full.repeats, full.build_repeats, full.cold_repeats), (2, 3, 0))

    def test_runner_matches_declared_matrix(self):
        # Exercise actual scheduling, mutations and post-maintenance branches.
        # The fake DB skips SQL execution; integration smoke runs cover SQL.
        for profile in ['focused', 'full']:
            for maintenance in [True, False]:
                with self.subTest(profile=profile, maintenance=maintenance):
                    args = self.args('--profile', profile, '--rows', '100', '--documents', '100',
                                     '--cold-repeats', '1', '--duration', '0',
                                     *([] if maintenance else ['--no-maintenance']))
                    runner = object.__new__(Suite)
                    runner.args = args
                    runner.cluster = Mock()
                    runner.cluster.db.scalar.return_value = '0'
                    runner.rng = random.Random(1)
                    runner.completed = set()
                    runner.log = runner.indexes = runner.visibility = runner.settings = Mock()
                    runner.plans = Mock()
                    runner.event = Mock()
                    runner.timed_operation = Mock(return_value=True)
                    runner.reference = lambda cases: {c.id: c.id for c in cases}
                    warm, cold = [], []

                    def measure(suite, n, variant, phase, cases, expected, modes=None, memory='64MB'):
                        for mode in modes:
                            for case in cases:
                                self.assertIn(case.id, expected)
                                warm.append((suite, n, variant, phase, mode, memory, case.id))

                    def cold_measure(suite, n, variant, cases):
                        cold.extend((suite, n, variant, 'shared_buffers_cold', 'default', '64MB', c.id)
                                    for c in cases)

                    runner.measure, runner.cold = measure, cold_measure
                    queries = {s: [c.record() for c in profile_cases(s, profile)]
                               for s in ['scalar', 'documents']}
                    queries['matrix'] = {s: measurement_matrix(s, profile, maintenance) for s in queries}
                    for suite in ['scalar', 'documents']:
                        runner.run_dataset(suite, 100)
                    expected_warm, expected_cold = expected_configurations({'arguments': vars(args)}, queries)
                    self.assertEqual(set(warm), expected_warm)
                    self.assertEqual(len(warm), len(expected_warm))
                    self.assertEqual(set(cold), expected_cold)
                    labels = [call.args[0] for call in runner.timed_operation.call_args_list]
                    self.assertEqual('reindex' in labels, maintenance and profile == 'full')
                    sql = [call.args[0] for call in runner.cluster.db.query.call_args_list]
                    self.assertEqual(any('SET payload=reverse(payload) WHERE id <=' in s for s in sql),
                                     profile == 'full')

    def test_full_matrix_matches_legacy_audit(self):
        args = self.args('--profile', 'full')
        queries = {s: [c.record() for c in profile_cases(s, 'full')] for s in ['scalar', 'documents']}
        legacy = expected_configurations({'arguments': vars(args)}, queries)
        # The saved matrix adds low-memory row fetches the legacy audit never had.
        variants = [(f, v) for f in args.families
                    for v in (['roaring', 'roaring_bitmap'] if f == 'roaring' else [f])]
        legacy[0].update(('scalar', n, v, 'low_work_mem', 'prefer_index', '64kB', case)
                         for n in args.rows for f, v in variants
                         for case in family_cases(f, LOW_MEMORY_FETCH))
        queries['matrix'] = {s: measurement_matrix(s, 'full') for s in queries}
        self.assertEqual(legacy, expected_configurations({'arguments': vars(args)}, queries))

    def test_index_portfolios(self):
        for family in ['btree', 'gin', 'roaring']:
            names = {name for name, _ in profile_indexes('scalar', family, 'focused')}
            self.assertEqual(names, {'ix_c2', 'ix_c20', 'ix_c200', 'ix_c20k', 'ix_c1m', 'ix_skew', 'ix_nullable'})
        self.assertEqual({name for name, _ in profile_indexes('documents', 'gin', 'focused')},
                         {'ix_tags', 'ix_tsv'})
        # roaring_btree: the roaring portfolio, and a B-tree on the ORDER BY column beside it
        specs = dict(profile_indexes('scalar', 'roaring_btree', 'focused'))
        self.assertEqual(set(specs), {'ix_c2', 'ix_c20', 'ix_c200', 'ix_c20k', 'ix_c1m', 'ix_skew',
                                      'ix_nullable', 'ix_c1m_btree'})
        self.assertIn('USING lion (c1m)', specs['ix_c1m'])
        self.assertIn('USING btree (c1m)', specs['ix_c1m_btree'])

    def test_ordered_cases(self):
        # Every family measures the ordered cases; roaring_btree measures only them, in the
        # clean phase, and has no document portfolio.
        for profile in ['focused', 'full']:
            args = self.args('--profile', profile)
            queries = {s: [c.record() for c in profile_cases(s, profile)] for s in ['scalar', 'documents']}
            queries['matrix'] = {s: measurement_matrix(s, profile) for s in queries}
            warm, cold = expected_configurations({'arguments': vars(args)}, queries)
            mine = {k for k in warm if k[2] == 'roaring_btree'}
            self.assertEqual({k[6] for k in mine}, set(ORDERED_CASES))
            self.assertEqual({(k[0], k[3]) for k in mine}, {('scalar', 'clean')})
            self.assertFalse(any(k[2] == 'roaring_btree' for k in cold))
            for family in ['btree', 'roaring', 'roaring_bitmap']:
                self.assertTrue(set(ORDERED_CASES) <= {k[6] for k in warm if k[2] == family})

    def test_historical_workloads_still_audit(self):
        directory = Path(__file__).resolve().parents[1] / 'results/2026-09-20-comparison'
        meta = json.loads((directory / 'metadata.json').read_text())
        queries = json.loads((directory / 'queries.json').read_text())
        warm, cold = expected_configurations(meta, queries)
        self.assertTrue(warm)
        self.assertTrue(cold)
        self.assertTrue(any(key[2] == 'brin' for key in warm))


if __name__ == '__main__':
    unittest.main()
