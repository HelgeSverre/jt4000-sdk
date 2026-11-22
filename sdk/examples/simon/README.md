# simon: a memory game with melodies and a talking opponent

The game Simon with a melody as the sequence. Every round the robot plays the melody with
one more note, and you play it back on the touch keys. The robot comments in speech.

```sh
make APP=simon flash      # on the synth
just host simon           # or in a window, without the synth
```

## Playing

| Control | Does |
|---|---|
| SELECT turn | Before a game: which game |
| SELECT press | Start. During your turn: hear the melody again, once a round (the robot says "again, really?") |
| Keys | Play. Any key also starts a game |
| LFO, FILTER, AMP, ARP | The instrument: pluck, bell, chip, organ |
| CUTOFF, RESONANCE | The analog filter, from the moment you move them |
| OSC | Give up, back to the start |

Switching on shows the robot's face, zooms into its mouth and dissolves into the game;
any touch skips it.

Three lives. A wrong note or running out of time (the line under the hearts) costs one,
and the robot plays the melody again. After your last correct note there is half a
second of quiet before it answers. Game over: it says your score and a verdict on it,
then the band plays the melody you built, repeating.

| Game | Rule |
|---|---|
| CLASSIC | The keys light as the robot plays them |
| BY EAR | Only the first note lights. The rest you hear |
| BACKWARDS | Play it last note first |

The best score of each game is kept through power-off.

## Melody, band and voice

- The notes come from one scale and move by a step or two from the note before, with an
  occasional leap, and some last twice as long. Five keys are in play at first (C D E G A),
  seven from round 6, every white key from round 12. The display draws the keyboard as it
  is printed on the synth; white keys in play carry a mark.
- A band plays under the melody, in A minor (A minor, F, C, G). It starts as a kick drum
  and gains a part every two rounds: hat, bass, clap, fast hats, pad. It also speeds up,
  from 99 to 144 beats a minute. The melody always starts on a beat.
- The robot sings some taunts on the notes of the melody; in others the pitch falls word
  by word or rises on the last word. Its face is pleased when you miss and sulks when you
  do not.

## The display

```
 face   what it says, or ROUND 7 / YOUR TURN
        lives   the melody as ticks (tall = played)
        time left
 the keyboard: marked = in play, filled = sounding, blinking = the one you missed
```

## Words

`words.txt` is what the robot can say; `just words simon --rate 150` rebuilds `words.h`
from it. Phrases are plain strings in `main.c` (`say("\\HA HA HA")`): a leading `\` makes
the pitch fall word by word, `?` lifts the last word, `~` sings it on the melody.
