# 07 — Media and the disc budget

Reproduce with:

```
python tools/disc_budget.py IMAGE.iso
python tools/media_census.py IMAGE.iso
```

Output: [`reports/disc-budget.txt`](../reports/disc-budget.txt),
[`reports/media-census.txt`](../reports/media-census.txt).

---

## Where 4,080,664,576 bytes went

Classified by each member's own first sixteen bytes, not by file name or
extension — the `AFS` directory truncates names at 32 characters, so the
extension is unreliable and keying on it misfiles thousands of members.

| By content | Bytes | Share |
|---|---:|---:|
| **`CDVD.000`** — unreferenced, see [08](08-cdvd000.md) | 1,073,676,288 | **26.31%** |
| video, Sofdec MPEG-2 | 890,421,248 | 21.82% |
| compressed game data, `CPS` | 826,535,905 | 20.25% |
| stored game data, `CPS` | 614,523,984 | 15.06% |
| audio, CRI ADX | 552,009,757 | 13.53% |
| voice, CRI AHX | 70,160,470 | 1.72% |
| archive overhead | 27,618,140 | 0.68% |
| executable | 2,972,228 | 0.07% |
| I/O processor modules | 982,469 | 0.02% |
| index and boot files | 99,731 | 0.00% |
| slack | 21,664,356 | 0.53% |
| | **4,080,664,576** | **100.00%** |

### Against the two builds either side of it

| Build | Media share | Of |
|---|---:|---|
| *Tales of Rebirth*, 2004 — video + voice | **67.20%** | 4.51 GB |
| *Tales of Tactics*, 2004 — all media | **63.30%** | 369 KB |
| ***Tales of Legendia*, 2005 — video + all audio** | **37.70%** | **4.08 GB** |
| the same, excluding `CDVD.000` from the denominator | **51.54%** | 2.99 GB |

**It breaks the pattern, and it breaks it downwards.** This was the number most
likely to go the other way. *Tales of Legendia* is known for its soundtrack, and
it is the only *Tales* title of its generation whose music came from outside the
series' usual staff — the disc confirms as much by shipping KORG's sound driver
rather than Sony's ([04](04-executable.md)). The expectation was that the audio
share would separate upward from its siblings. It separates downward, and the
reason is architectural rather than a matter of how much music there is.

### Because most of the music is not audio

Splitting the audio by what it actually is rather than by which archive it sits
in — cast-prefixed members counted as voice, `SND.AFS`'s `CPS` members as the
sound driver's own data:

| | Members | Bytes | Share of disc |
|---|---:|---:|---:|
| voice, ADX under a cast prefix | 15,388 | 329,675,319 | 8.08% |
| music and ambience, streamed ADX | 255 | 222,334,438 | 5.45% |
| voice, AHX | 2,739 | 70,160,470 | 1.72% |
| sample banks and sound data, `CPS` | 393 | 25,811,613 | 0.63% |
| **MIDI sequences, `CPS`** | **30** | **136,763** | **0.003%** |
| | **18,805** | **648,118,603** | **15.88%** |

**Thirty of this disc's files are MIDI sequences and together they are 136,763
bytes.** `BATTLE_ZAKO_MIDI`, `BATTLE_BOSS_MIDI`, `BATTLE_WALT_MIDI`,
`FIELD_MIDI`, `TOGIJO_MIDI`, `ISOGE_MIDI`, `SHINKOKU2_MIDI` — battle themes,
the field theme, and named cues. They are sequences, not recordings. What plays
them is the KORG sound driver on the I/O processor, out of the 25.8 MB of sample
banks beside them.

So a substantial part of the score costs the disc **26 MB, six tenths of one
percent**, because it is sequenced rather than streamed. *Tales of Rebirth* put
1.30 GB of SPU-ADPCM on its disc. A reputation for a large soundtrack and a
small audio budget are not in tension here; they are the same fact seen from two
sides, and the disc is the only place the distinction is visible.

**What is streamed is mostly voice.** 399.8 MB — 9.80% of the disc — is
cast-prefixed ADX and AHX, against 222.3 MB of streamed music and ambience. By
the ADX headers' own sample counts the total ADX playing time is **13.97 hours**:
15,643 streams, 15,531 of them mono, 15,578 at 24 kHz.

**And a quarter of the denominator is dead weight.** `CDVD.000` is not content.
Excluding it raises media to 51.54%, which is the more honest comparison and
still well below both siblings.

---

## Video

Twenty-two Sofdec streams in `MOVIE.AFS`, every one **640 × 320 at 29.97 fps** —
a 2:1 frame, not *Rebirth*'s 640 × 448. Three bit rates, and they group by
number:

| Streams | Declared bit rate |
|---|---|
| `mov_01` … `mov_19` (17 files) | 4,111,200 bit/s |
| `mov_20` … `mov_23` (4 files) | 4,500,000 bit/s |
| `mov_24` | 3,600,000 bit/s |

The largest is `mov_15` at 235,921,408 bytes and the second `mov_24` at
177,399,808 — together 46.4% of all the video on the disc.

**The numbering has two gaps.** The files run `mov_01`, `mov_02`, `mov_04` …
`mov_15`, `mov_17` … `mov_24`. There is no `mov_03` and no `mov_16`, in a set
that is otherwise contiguous to 24. Twenty-four slots, twenty-two shipped.
[09](09-leftovers.md).

---

## Audio, and a cast list

Three CRI formats, separated by magic:

| Format | Members | Bytes | What it is |
|---|---:|---:|---|
| ADX | 15,643 | 552,009,757 | CRI ADPCM — voice, music and ambience |
| AHX | 2,739 | 70,160,470 | CRI MPEG-2 layer II voice, in a `TLPS` chunk |
| Sofdec | 22 | 890,421,248 | MPEG-2 program streams |

ADX parameters, read from the headers rather than assumed: **24,000 Hz on 15,578
streams and 48,000 Hz on 65; mono on 15,531 and stereo on 112**. Total declared
playing time **50,289.0 seconds = 13.97 hours**.

The AHX members are wrapped in a four-byte `TLPS` chunk that is not CRI's — the
same two-letter prefix as the `TLPKv1+50` tag found inside `CDVD.000`. `TL` is
presumably *Tales of Legendia*, matching the `TOL` in the memory-card string
([09](09-leftovers.md)).

### `SND.AFS` is named by character

Its members are named by a short prefix, and the prefixes are the party. Counting
members and ADX seconds per prefix turns 16,261 files into a cast list:

| Prefix | ADX | AHX | Bytes | ADX seconds | Reads as |
|---|---:|---:|---:|---:|---|
| `VO` | 2,192 | 0 | 51,123,134 | 3,776.1 | battle voice |
| `NOR` | 1,834 | 348 | 50,085,744 | 5,423.4 | **Norma** |
| `SEN` | 2,042 | 464 | 43,419,416 | 4,468.6 | **Senel** |
| `WIL` | 1,307 | 206 | 34,631,328 | 3,792.5 | **Will** |
| `CHL` | 1,156 | 333 | 33,263,622 | 3,376.5 | **Chloe** |
| `JAY` | 1,148 | 147 | 30,763,429 | 3,761.9 | **Jay** |
| `MOS` | 1,181 | 171 | 27,779,690 | 3,247.0 | **Moses** |
| `SHI` | 910 | 369 | 25,077,272 | 2,142.4 | **Shirley** |
| `GUR` | 424 | 141 | 14,126,931 | 1,448.0 | **Grune** |
| `HAR` | 362 | 83 | 8,905,404 | 1,007.8 | **Harriet** |
| `SCH` | 114 | 64 | 6,906,543 | 610.5 | **Schwartz** |
| `VAC` | 147 | 0 | 4,300,577 | 24.9 | **Vaclav** |

**The protagonist is not the most-recorded character.** `NOR` — Norma — has
2,182 members and 5,423 seconds against Senel's 2,506 and 4,469, and more bytes
than him. On a disc that names its files by speaker, the supporting character
with the most lines is measurable, and it is not the lead.

The rest of the archive is ambience and music under descriptive names of their
own: `SND_MUSIC_0000000`, `SND_ENVIRONMENTAL_SOUND_BEACH`,
`SND_COMMON_GLOBAL_FLOOR00`, and a long tail of single-member prefixes that read
as cue names — `KINPAKU` (緊迫, tension) with five numbered variants, `FUAN`
(不安, unease), `MORIAGARI` (盛り上がり, build-up) with two, `COMICAL` with
three, `KANK` with three, and `YOBI` (予備, spare) — of which `YOBI00` to
`YOBI09` are ADX and `YOBI20` to `YOBI25` are MIDI sequences, with nothing
numbered in between.

**Twenty members named `DANG00_24K.adx` through `DANG19_24K.adx` are 57,344
bytes each and byte-for-byte identical to one another** — twenty copies of one
two-second stream, 1.1 MB of the disc, under twenty different names.

`MBS/DUMMY.MBS` and `AFS/DUMMY.AFS` are both fifty bytes and **byte-for-byte
identical to each other** — a `CPS` header declaring 50 packed and 128 unpacked,
followed by a token stream. Two placeholder files, in two directories, from one
source.
