# DVD-Video logo animation

The idle screen is a behavior-level recreation of the familiar bouncing
DVD-Video logo. It is not a decompilation of a specific player's
screensaver; no public source or decompilation of the original
implementation was found.

## Behavior

- Official DVD-Video logo artwork on a black background.
- Constant-speed diagonal movement, advanced using elapsed time.
- The logo's rectangular bounds bounce independently from the screen edges.
- The tint changes on each horizontal or vertical wall impact.
- A large frame-time pause is clamped to keep the logo from visibly jumping.

The classic diagonal/bounce model is described in
[The Lost Math Lessons: Bouncing DVD Logo](http://lostmathlessons.blogspot.com/2016/03/bouncing-dvd-logo.html).
That describes the recognizable motion, not any one manufacturer's exact
speed, initial position, or color sequence.

The animation is drawn while no disc playback is active and is replaced by
video as soon as the first frame is presented.
