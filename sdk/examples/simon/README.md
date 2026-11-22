# simon: the robot plays, you repeat, it gloats

Simon, but the sequence is a tune. Every round the melody grows by one note. You play it
back on the touch keys. The robot comments, and it is not a good loser.

```sh
make APP=simon flash      # on the synth
just host simon           # or in a window, without the synth
```

## Playing

| Control | Does |
|---|---|
| SELECT turn | Before a game: which game |
| SELECT press | Start. During your turn: hear the melody again, once a round (the robot will have an opinion) |
| Keys | Play. Any key also starts a game |
| LFO, FILTER, AMP, ARP | The instrument: pluck, bell, chip, organ |
| CUTOFF, RESONANCE | The analog filter, from the moment you move them |
| OSC | Give up, back to the start |

Three lives. A wrong note or running out of time (the line under the hearts) costs one,
and the robot plays the melody again. Game over: it says your score and what it thinks of
it, then the band plays the melody you built, round and round.

| Game | Rule |
|---|---|
| CLASSIC | The keys light as the robot plays them |
| BY EAR | Only the first note lights. The rest you hear |
| BACKWARDS | Play it last note first |

The best score of each game is kept through power-off.

## What makes it musical

- **The notes are a tune.** They come from one scale and move by a step or two from the
  note before, with an occasional leap, and some last twice as long. Five keys are in play
  at first (C D E G A), seven from round 6, every white key from round 12. The display
  draws the keyboard as it is printed on the synth; white keys in play carry a mark.
- **A band plays under it**, in A minor (A minor, F, C, G). It starts as a kick drum and
  gains a part every two rounds: hat, bass, clap, fast hats, pad. It also speeds up, from
  99 to 144 beats a minute. The melody always starts on a beat.
- **The robot sings.** Some taunts are sung on the notes of the melody, others fall or
  rise like a sneer or a question. Its face is pleased when you miss and sulks when you
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
