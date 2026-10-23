# Test data and licenses

The bench builds its test calls from two public datasets. Neither is stored in
this repo; `scripts/fetch_data.sh` downloads them.

| Dataset | Used for | License | Commercial use |
|---|---|---|---|
| [ESC-50](https://github.com/karolpiczak/ESC-50) (K. J. Piczak) | Coughs, sneezes, and look-alike sounds (laughing, clapping, knocking, typing, breathing, mouse clicks) | CC BY-NC 3.0 | **No.** Fine for testing and research; do not ship it or train a commercial model on it. |
| [Free Spoken Digit Dataset](https://github.com/Jakobovski/free-spoken-digit-dataset) | The talker's voice | CC BY-SA 4.0 | Yes, with attribution and share-alike |

Before training a model you intend to sell or license, replace ESC-50 with
audio you have commercial rights to (your own recordings with consent, or a
commercially licensed set).

## How the test calls are made

* ~30 s each, one speaker, mono 16 kHz.
* 2–3 coughs or sneezes per call, at known times, 4–14 dB louder than the voice.
* 1–2 look-alike sounds per call that must stay audible.
* About half the events land in pauses, half over speech.
* Half the calls have background noise (−55 to −40 dBFS).
* **dev** and **test** never share a speaker or a source clip (ESC-50 folds 1–4
  vs fold 5; speakers jackson/nicolas/theo/yweweler vs george/lucas). Tune on
  dev; report test.

Every clip, event, speech span, and scoring run is recorded in
`data/catalog.db` (SQLite).
