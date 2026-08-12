#!/usr/bin/env python3

import importlib.util
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "benchmark_local", REPO / "tools" / "benchmark_local.py"
)
benchmark = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(benchmark)


class LocalBenchmarkTests(unittest.TestCase):
    def passing_values(self):
        return {
            name: rule["threshold"]
            for name, rule in benchmark.LIMITS.items()
        }

    def test_every_threshold_value_passes(self):
        self.assertTrue(all(benchmark.evaluate(self.passing_values()).values()))

    def test_minimum_and_maximum_regressions_fail(self):
        values = self.passing_values()
        values["queue_operations_per_second"] -= 1
        values["peak_rss_kib"] += 1
        checks = benchmark.evaluate(values)
        self.assertFalse(checks["queue_operations_per_second"])
        self.assertFalse(checks["peak_rss_kib"])

    def test_missing_or_unknown_metrics_fail_closed(self):
        values = self.passing_values()
        values.pop("cli_status_p95_ms")
        with self.assertRaisesRegex(benchmark.BenchmarkError, "supported limit set"):
            benchmark.evaluate(values)


if __name__ == "__main__":
    unittest.main()
