#!/usr/bin/env python3

import importlib.util
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock


REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "lez_bond_matrix", REPO / "tools" / "lez_bond_matrix.py"
)
matrix = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(matrix)


class BondMatrixTests(unittest.TestCase):
    def args(self, root: Path):
        return types.SimpleNamespace(
            wallet_source=Path("wallet-source"),
            wallet_home=Path("wallet-home"),
            sender="sender",
            owner="owner",
            sink="sink",
            release_commit="12" * 20,
            amount=10,
            standard_validity_ms=100_000,
            expiry_delay_ms=20_000,
            timeout=21600,
            submit=False,
            wait_for_expiry=False,
            journal=root / "matrix.json",
            candidate_dir=root / "candidates",
        )

    def test_plan_has_fresh_stable_ids_and_expiry_specific_deadline(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.args(Path(directory))
            plan = matrix.build_plan(args, 1000)
            self.assertEqual(set(plan["cases"]), {case for case, _ in matrix.CASES})
            ids = [values["bond_id"] for values in plan["cases"].values()]
            self.assertEqual(len(ids), len(set(ids)))
            self.assertTrue(all(matrix.HEX_32.fullmatch(value) for value in ids))
            self.assertEqual(plan["cases"]["expiry"]["deadline_ms"], 21_000)
            self.assertEqual(plan["cases"]["acceptance"]["deadline_ms"], 101_000)

    def test_command_is_sequential_typed_and_submission_gated(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.args(Path(directory))
            values = matrix.build_plan(args, 1000)["cases"]["acceptance"]
            command = matrix.command(args, "acceptance", "initialize", values)
            self.assertNotIn("--submit", command)
            self.assertIn("--deadline-ms", command)
            args.submit = True
            command = matrix.command(args, "acceptance", "settle", values)
            self.assertIn("--submit", command)
            self.assertEqual(command[command.index("--outcome") + 1], "refund-accepted")

    def test_expired_initialize_and_early_expiry_stop_before_subprocess(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = self.args(root)
            plan = matrix.build_plan(args, 1000)
            matrix.lez_wallet.atomic_json(args.journal, plan)
            with mock.patch.object(matrix.time, "time", return_value=200):
                with mock.patch.object(matrix.subprocess, "run") as run:
                    with self.assertRaisesRegex(matrix.MatrixError, "validity window"):
                        matrix.execute(args)
            run.assert_not_called()

            plan = matrix.build_plan(args, 1000)
            plan["completed_steps"] = [
                "expiry-initialize",
                "acceptance-initialize",
                "acceptance-settle",
                "rejection-initialize",
                "rejection-settle",
                "delivery-failure-initialize",
                "delivery-failure-settle",
            ]
            for case, operation, outcome in matrix.STEPS:
                step = f"{case}-{operation}"
                if step not in plan["completed_steps"]:
                    continue
                path = Path(plan["cases"][case]["candidate_paths"][operation])
                path.parent.mkdir(parents=True, exist_ok=True)
                matrix.lez_wallet.atomic_json(
                    path,
                    {
                        "status": "official-wallet-sequencer-finalized-candidate",
                        "operation": operation,
                        "outcome": outcome,
                        "transaction_type": "PrivacyPreserving",
                        "finality": "Finalized",
                    },
                )
            matrix.lez_wallet.atomic_json(args.journal, plan)
            with mock.patch.object(matrix.time, "time", return_value=2):
                with self.assertRaisesRegex(matrix.MatrixError, "early"):
                    matrix.execute(args)


if __name__ == "__main__":
    unittest.main()
