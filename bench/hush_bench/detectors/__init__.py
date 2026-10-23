"""Detectors plug into the bench through one small interface.

A detector looks at a whole clip but must behave as if it were live: for
every mute it proposes it also says WHEN it made the decision (the time of the
newest audio sample it needed). The scorer uses that to enforce the look-ahead
budget, so a detector can't cheat by peeking further ahead than the engine's
buffer allows.
"""
from dataclasses import dataclass


@dataclass
class Mute:
    start_s: float      # when the detector wants the mute to begin
    end_s: float        # when it wants to unmute
    decided_s: float    # newest audio it had to hear to make the call
    kind: str = ""      # its guess: cough / sneeze


class Detector:
    name = "base"

    def params(self) -> dict:
        return {}

    def mutes(self, pcm, sr) -> list[Mute]:
        raise NotImplementedError


def get(name, **overrides):
    if name == "rules":
        from .rules import RulesDetector
        return RulesDetector(**overrides)
    if name == "engine":
        from .engine import EngineDetector
        return EngineDetector(**overrides)
    if name == "model":
        from .model import ModelDetector
        return ModelDetector(**overrides)
    if name == "never":
        return Never()
    raise ValueError(f"unknown detector: {name}")


class Never(Detector):
    """Baseline that never mutes. Every cough leaks; nothing is cut."""
    name = "never"

    def mutes(self, pcm, sr):
        return []
