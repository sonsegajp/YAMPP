"""Telling the player how far along a long job is.

Preparing Akaneia takes minutes: it patches a 1.4 GB disc image, extracts it,
composes the content and verifies the result. Until now the only thing on
screen was a caption and a bar that sat at zero and then jumped to done, which
is indistinguishable from a hang -- and a player who cannot tell a slow job
from a dead one will kill it, usually most of the way through.

The worker runs as a separate process and reports its final answer in a JSON
file. Progress needs to arrive while it is still working, so it goes to a
small sidecar next to that file, rewritten atomically as each stage begins.
The runtime reads whatever is there on its own schedule; a missed or
half-written update costs nothing, because the next one supersedes it.

Percentages are assigned per stage from measured proportions of a real import
rather than spread evenly, because the stages differ by more than an order of
magnitude in duration and an even split would stall visibly at the long ones.
"""
import json
import os
import tempfile
from pathlib import Path

# Where each stage ends, as a percentage. The gaps are the measured share of a
# real Akaneia import: patching and extracting dominate it.
STAGES = {
    "verify": (0, 5, "Verifying the official archive"),
    "download": (5, 30, "Downloading the official archive"),
    "metadata": (30, 32, "Fetching upstream metadata"),
    "unpack": (32, 38, "Unpacking the official patch"),
    "patch": (38, 62, "Applying the official patch to your disc"),
    "extract": (62, 78, "Extracting the patched disc"),
    "compose": (78, 92, "Importing fighters, stages and music"),
    "check": (92, 99, "Verifying the imported content"),
    "done": (100, 100, "Content ready"),
}


class Progress:
    """Writes stage progress where the runtime can read it."""

    def __init__(self, path):
        self.path = Path(path) if path else None
        self.stage = None
        self.percent = 0

    def _write(self, percent, message):
        if self.path is None:
            return
        percent = max(0, min(100, int(percent)))
        # Never go backwards: a later stage reporting a stale byte count would
        # otherwise make the bar jump about.
        if percent < self.percent:
            percent = self.percent
        self.percent = percent
        payload = json.dumps({"schema": 1, "percent": percent, "message": message})
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            fd, name = tempfile.mkstemp(prefix=".progress-", dir=str(self.path.parent))
            temporary = Path(name)
            try:
                with os.fdopen(fd, "w", encoding="utf-8") as out:
                    out.write(payload)
                temporary.replace(self.path)
            finally:
                temporary.unlink(missing_ok=True)
        except OSError:
            # Progress is a courtesy; failing to report it must never fail the
            # job the player actually asked for.
            pass

    def begin(self, stage):
        low, _, message = STAGES.get(stage, (self.percent, self.percent, stage))
        self.stage = stage
        self._write(low, message)

    def within(self, stage, done, total):
        """Position inside a stage, for the one step that can measure itself."""
        low, high, message = STAGES.get(stage, (self.percent, self.percent, stage))
        fraction = (done / total) if total else 0.0
        self._write(low + (high - low) * min(1.0, max(0.0, fraction)), message)

    def finish(self):
        self.begin("done")


def sidecar(output):
    """The progress file that belongs beside a worker's output file."""
    return Path(str(output) + ".progress") if output else None
