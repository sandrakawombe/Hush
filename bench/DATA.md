# Test data and licenses

The bench builds its test calls from two public datasets. Neither is stored in
this repo; `scripts/fetch_data.sh` downloads them.

| Dataset | Used for | License | Commercial use |
|---|---|---|---|
| [ESC-50](https://github.com/karolpiczak/ESC-50) (K. J. Piczak) | Coughs, sneezes, and look-alike sounds (laughing, clapping, knocking, typing, breathing, mouse clicks) | CC BY-NC 3.0 | **No.** Fine for testing and research; do not ship it or train a commercial model on it. |
| [Free Spoken Digit Dataset](https://github.com/Jakobovski/free-spoken-digit-dataset) | The talker's voice | CC BY-SA 4.0 | Yes, with attribution and share-alike |
| [Coswara](https://github.com/iiscleap/Coswara-Data) (Indian Institute of Science) | Model training: coughs, counting out loud, breathing and vowels from 227 people | CC BY 4.0 | **Yes**, with attribution |

`scripts/fetch_coswara.sh` downloads two days of Coswara recordings (237
people) with a sparse checkout; the full repository is many gigabytes.

**Licensing of the current model.** `models/hush_tcn_v2.npz` was trained on
ESC-50 as well as Coswara and FSDD, so it inherits ESC-50's non-commercial
terms. It is fine for development, demos and research. Before selling or
licensing a model, retrain it without ESC-50: Coswara covers coughs, but a
commercially usable source of **sneezes** is still needed (ESC-50 is the only
sneeze source today).

## Model training splits

`train/gen_data.py` builds 10-second training calls. Splits never share a
person or a source clip, and nothing from the bench's locked test set is used:

| Split | ESC-50 | FSDD speakers | Coswara people |
|---|---|---|---|
| train | folds 1–3 | jackson, nicolas, theo | 8 in every 10 (183) |
| val (tuning) | fold 4 | yweweler | 1 in every 10 (22) |
| held out | – | – | 1 in every 10 (22), for a future realistic test set |
| bench test (locked) | fold 5 | george, lucas | none |

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
