"""Regressions for the progress a long import reports while it runs.

Run from the repository root:
    python -m unittest discover -s tools/modkit -p "test_*.py"
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from job_progress import Progress, STAGES, sidecar

ORDER = ["verify", "download", "metadata", "unpack", "patch", "extract", "compose", "check"]


class ProgressTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = Path(self.dir.name) / "worker.json.progress"
        self.progress = Progress(self.path)

    def tearDown(self):
        self.dir.cleanup()

    def read(self):
        return json.loads(self.path.read_text())

    def test_the_sidecar_sits_beside_the_workers_output(self):
        self.assertEqual(sidecar("/tmp/worker-1.json").name, "worker-1.json.progress")
        self.assertIsNone(sidecar(None))

    def test_every_stage_reports_a_position_and_something_to_read(self):
        for stage in ORDER:
            self.progress.begin(stage)
            value = self.read()
            self.assertTrue(0 <= value["percent"] <= 100)
            self.assertTrue(value["message"].strip())
            self.assertEqual(value["schema"], 1)

    def test_progress_only_ever_moves_forward(self):
        # A bar that jumps backwards reads as a fault even when the job is
        # healthy, and an in-stage report can arrive after the next stage has
        # already begun.
        seen = []
        for stage in ORDER:
            self.progress.begin(stage)
            seen.append(self.read()["percent"])
        self.assertEqual(seen, sorted(seen))
        self.progress.within("download", 1, 100)      # a stale, much earlier report
        self.assertGreaterEqual(self.read()["percent"], seen[-1])

    def test_the_download_stage_tracks_bytes_within_its_own_span(self):
        low, high, _ = STAGES["download"]
        self.progress.begin("download")
        self.assertEqual(self.read()["percent"], low)
        self.progress.within("download", 1, 2)
        half = self.read()["percent"]
        self.assertTrue(low < half < high, "%d not between %d and %d" % (half, low, high))
        self.progress.within("download", 2, 2)
        self.assertEqual(self.read()["percent"], high)

    def test_a_zero_length_download_does_not_divide_by_zero(self):
        self.progress.begin("download")
        self.progress.within("download", 0, 0)
        self.assertEqual(self.read()["percent"], STAGES["download"][0])

    def test_finishing_reaches_a_hundred(self):
        self.progress.begin("patch")
        self.progress.finish()
        self.assertEqual(self.read()["percent"], 100)

    def test_an_unknown_stage_holds_position_rather_than_guessing(self):
        self.progress.begin("compose")
        held = self.read()["percent"]
        self.progress.begin("something-new")
        self.assertEqual(self.read()["percent"], held)

    def test_reporting_is_a_courtesy_and_never_fails_the_job(self):
        # No destination at all, and a destination that cannot be written.
        Progress(None).begin("patch")
        blocked = Progress(Path(self.dir.name) / "worker.json" / "nested" / "p.json")
        blocked.begin("patch")
        blocked.within("download", 1, 2)
        blocked.finish()

    def test_each_write_leaves_a_complete_document(self):
        # The runtime reads this file at any moment, so a partial write would
        # be parsed as garbage; the writer replaces it atomically instead.
        for stage in ORDER:
            self.progress.begin(stage)
            self.assertIsInstance(self.read(), dict)
        self.assertEqual(sorted(Path(self.dir.name).glob(".progress-*")), [])


if __name__ == "__main__":
    unittest.main()
