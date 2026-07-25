from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_notice_id_zero_clears_staged_notice() -> None:
    source = (ROOT / "src" / "state_store.c").read_text(encoding="utf-8")
    assert "state->notice.active = notice_id != 0;" in source
