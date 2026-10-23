"""Checks that the scoring math says what it claims, on hand-made cases."""
import numpy as np

from hush_bench import SR, catalog, score
from hush_bench.detectors import Mute


def ev(onset, offset, target=True, kind="coughing", over=False, id_=1):
    return {"id": id_, "kind": kind, "is_target": target, "onset_s": onset, "offset_s": offset, "over_speech": over}


def test_perfect_mute_is_caught_with_no_leak():
    (e, cov, leak), = score.score_clip([ev(1.0, 1.5)], [], [(0.97, 1.6)])[0]
    assert cov == 1.0 and leak == 0.0


def test_late_mute_leaks_and_misses():
    (e, cov, leak), = score.score_clip([ev(1.0, 1.5)], [], [(1.1, 1.6)])[0]
    assert abs(leak - 100) < 1e-6
    assert abs(cov - 2 / 3) < 1e-6  # 200 of the 300 ms attack covered -> below 80%


def test_unrelated_mute_is_false_and_cuts_speech():
    per, flags, cut = score.score_clip([ev(1.0, 1.5)], [(3.0, 4.0)], [(3.2, 3.6)])
    assert flags == [(3.2, 3.6, True)]
    assert abs(cut - 0.4) < 1e-6


def test_mute_on_a_lookalike_counts_as_false():
    per, flags, _ = score.score_clip([ev(2.0, 2.4, target=False, kind="laughing")], [], [(1.98, 2.5)])
    assert flags[0][2] is True
    assert per[0][1] >= score.LOOKALIKE_AT


def test_lookahead_budget_pushes_start_later():
    m = [Mute(start_s=0.97, end_s=1.6, decided_s=1.072)]
    assert score.apply_budget(m, 0.040, 10) == [(1.032, 1.6)]
    assert score.apply_budget(m, 0.200, 10) == [(0.97, 1.6)]


def test_never_detector_scores_zero(tmp_path):
    con = catalog.connect(tmp_path / "t.db")
    p = tmp_path / "c.wav"
    from hush_bench import audio
    audio.write(p, np.zeros(SR, np.float32))
    catalog.add_clip(con, "c", "dev", str(p), 1.0, "x", None, [
        {"kind": "coughing", "is_target": True, "onset_s": 0.2, "offset_s": 0.5, "over_speech": False, "source": "t"}], [])
    con.commit()
    catalog.DB_PATH, old = tmp_path / "t.db", catalog.DB_PATH
    try:
        _, m = score.run("never", "dev", 40, {}, save=False, con=con)
    finally:
        catalog.DB_PATH = old
    assert m["caught_pct"] == 0.0 and m["false_mutes"] == 0
