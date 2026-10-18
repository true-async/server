#!/usr/bin/env python3
"""Exercise CI gate failures independently of the live server."""
import copy
import json
import unittest
from http11probe_gate import POLICY, validate


class GateTests(unittest.TestCase):
    def setUp(self):
        self.policy = json.loads(POLICY.read_text())
        self.data = {'results': [dict(id=k, scored=v, verdict='Pass', statusCode=200)
                                 for k, v in self.policy['tests'].items()]}

    def test_complete_run_and_legal_warning(self):
        self.data['results'][0]['verdict'] = 'Warn'
        self.assertEqual([], validate(self.data, self.policy))

    def test_missing_duplicate_unknown_and_changed_scoring(self):
        variants = [self.data['results'][1:], self.data['results'] + [self.data['results'][0]]]
        unknown = copy.deepcopy(self.data['results'])
        unknown[0]['id'] = 'UNKNOWN'
        variants.append(unknown)
        changed = copy.deepcopy(self.data['results'])
        changed[0]['scored'] = not changed[0]['scored']
        variants.append(changed)
        for results in variants:
            with self.subTest(results=results[0]), self.assertRaises(ValueError):
                validate({'results': results}, self.policy)

    def test_only_exact_413_exception(self):
        row = next(r for r in self.data['results'] if r['id'] == 'MAL-CHUNK-EXT-64K')
        row.update(verdict='Fail', statusCode=413)
        self.assertEqual(['MAL-CHUNK-EXT-64K'], validate(self.data, self.policy))
        for status in [400, 500, None]:
            row['statusCode'] = status
            with self.subTest(status=status), self.assertRaises(ValueError):
                validate(self.data, self.policy)
        row.update(verdict='Pass', statusCode=200)
        other = next(r for r in self.data['results'] if r['scored'] and r is not row)
        other.update(verdict='Fail', statusCode=413)
        with self.assertRaises(ValueError):
            validate(self.data, self.policy)

    def test_unscored_fail_informational_but_error_skip_rejected(self):
        row = next(r for r in self.data['results'] if not r['scored'])
        row['verdict'] = 'Fail'
        validate(self.data, self.policy)
        for verdict in ['Error', 'Skip', 'Unknown', None]:
            row['verdict'] = verdict
            with self.subTest(verdict=verdict), self.assertRaises(ValueError):
                validate(self.data, self.policy)

    def test_missing_array_and_empty_run_rejected(self):
        for data in [{}, {'results': []}, {'results': None}, {'results': [None]}]:
            with self.subTest(data=data), self.assertRaises(ValueError):
                validate(data, self.policy)


if __name__ == '__main__':
    unittest.main()
