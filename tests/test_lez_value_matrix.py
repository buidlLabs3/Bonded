#!/usr/bin/env python3

import importlib.util
import json
import os
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock


REPO = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "lez_value_matrix", REPO / "tools" / "lez_value_matrix.py"
)
matrix = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(matrix)


class ValueMatrixTests(unittest.TestCase):
    SENDER = "6oFNU77YaPqJVB69t1rPwGdYeULXixugNR9KJiNocWLR"
    OWNER = "EsxbHEvBVTKAcz2vPq46NSi1AG9pbVYk65LyNfRQknZV"
    SINK = "FW6NaTcfeb354webZg3UqDLMVUFKv166mGTX6bxGswua"

    def args(self, root: Path):
        return types.SimpleNamespace(
            network_profile=REPO / "config/lez-testnet-network.json",
            wallet_source=Path("wallet-source"),
            wallet_home=Path("wallet-home"),
            sender=self.SENDER,
            owner=self.OWNER,
            sink=self.SINK,
            release_commit="12" * 20,
            timeout=21600,
            prover="ipc",
            rayon_threads=4,
            submit=False,
            authorization_dir=REPO / "evidence/testnet/authorizations",
            candidate_dir=root / "candidates",
            journal=root / "value-matrix.json",
        )

    def test_plan_binds_all_signed_release_operations(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.args(Path(directory))
            plan = matrix.build_plan(args, 1786700000000)
            self.assertEqual(
                list(plan["operations"]),
                [operation for operation, _role, _amount in matrix.OPERATION_SPECS],
            )
            self.assertEqual(plan["execution"]["rayon_num_threads"], 4)
            self.assertEqual(
                plan["operations"]["owner-approved-transfer"]["recipient"],
                self.OWNER,
            )
            self.assertEqual(
                plan["operations"]["paid-task-settlement"]["recipient"],
                self.SINK,
            )

    def test_command_is_typed_gated_and_execution_bound(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.args(Path(directory))
            values = matrix.build_plan(args, 1786700000000)["operations"][
                "below-limit-transfer"
            ]
            command = matrix.command(args, values)
            self.assertNotIn("--submit", command)
            self.assertEqual(command[command.index("--amount") + 1], "2")
            self.assertEqual(command[command.index("--prover") + 1], "ipc")
            self.assertEqual(command[command.index("--rayon-threads") + 1], "4")
            args.submit = True
            self.assertIn("--submit", matrix.command(args, values))

    def test_submission_gate_runs_before_writing_or_spawning(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.args(Path(directory))
            args.submit = True
            with mock.patch.dict(os.environ, {}, clear=True):
                with mock.patch.object(matrix.subprocess, "run") as run:
                    with self.assertRaisesRegex(matrix.ValueMatrixError, "requires"):
                        matrix.execute(args)
            run.assert_not_called()
            self.assertFalse(args.journal.exists())

    def test_completed_candidates_are_an_ordered_resumable_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = self.args(root)
            args.submit = True

            def run(command, cwd):
                evidence = Path(command[command.index("--evidence") + 1])
                operation = command[command.index("--operation") + 1]
                plan = json.loads(args.journal.read_text(encoding="utf-8"))
                values = plan["operations"][operation]
                evidence.parent.mkdir(parents=True, exist_ok=True)
                matrix.lez_wallet.atomic_json(
                    evidence,
                    {
                        "status": "official-wallet-sequencer-finalized-candidate",
                        "operation": operation,
                        "network": "lez-testnet",
                        "profile": "settlement",
                        "accounts": {
                            "sender": values["sender"],
                            "recipient": values["recipient"],
                        },
                        "amount": values["amount"],
                        "authorization_sha256": values["authorization_sha256"],
                        "trusted_signers_sha256": values[
                            "trusted_signers_sha256"
                        ],
                        "execution": plan["execution"],
                        "transaction_type": "PrivacyPreserving",
                        "finality": "Finalized",
                        "transaction": "ab" * 32,
                    },
                )
                return types.SimpleNamespace(returncode=0)

            with mock.patch.dict(
                os.environ,
                {"BONDED_LEZ_SUBMIT": "YES", "RISC0_DEV_MODE": "0"},
                clear=True,
            ):
                with mock.patch.object(matrix.time, "time", return_value=1786700000):
                    with mock.patch.object(
                        matrix.lez_value, "verify_signature", return_value=True
                    ):
                        with mock.patch.object(
                            matrix.subprocess, "run", side_effect=run
                        ):
                            result = matrix.execute(args)
            self.assertEqual(result["status"], "completed")
            self.assertEqual(
                result["completed_operations"],
                [operation for operation, _role, _amount in matrix.OPERATION_SPECS],
            )
            result["completed_operations"] = ["owner-approved-transfer"]
            matrix.lez_wallet.atomic_json(args.journal, result)
            with mock.patch.dict(
                os.environ,
                {"BONDED_LEZ_SUBMIT": "YES", "RISC0_DEV_MODE": "0"},
                clear=True,
            ):
                with mock.patch.object(matrix.time, "time", return_value=1786700000):
                    with mock.patch.object(
                        matrix.lez_value, "verify_signature", return_value=True
                    ):
                        with self.assertRaisesRegex(
                            matrix.ValueMatrixError, "ordered prefix"
                        ):
                            matrix.execute(args)


if __name__ == "__main__":
    unittest.main()
