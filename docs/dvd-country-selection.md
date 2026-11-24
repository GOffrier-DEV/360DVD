# Main-disc DVD trace: France vs. United Kingdom

The country picker is an authored DVD menu, not a 360DVD language setting.
It appears on an indefinite DVD still (`DVDNAV_STILL_FRAME`, length `255`).
Pressing **A** executes the selected button's DVD VM command. The disc can
choose a different warning sequence, menu, or stream settings for each country.
There is no requirement for the two buttons to visit the same PGCs.

| Selection | What the console trace showed |
| --- | --- |
| **United Kingdom** | Button **1** set `GPRM[15]` from `99` to `1` and linked to the tail of the picker PGC. Its post-commands tested the country value, cleared the register to `0`, and took the menu jump. The next playable cell was **VTS 1, menu PGC 1, cell 1** (the 30.840-second main menu). No separate warning cell was entered between the selection and that menu in this capture. |
| **France** | Button **10** entered the French warning path. The trace recorded a **15-second authored still**, then later `JumpTT 9`, entering **VTS 3, title 9, PGC 4**. The VM then returned to **VTS 1, menu PGC 1**. The console played the localized warnings in order, and FFmpeg identified the selected audio as French on the warning title and on the feature in the French run. |

The UK result is a direct jump **made by the disc's VM**, not an ad-skip rule
in the player. Both paths reach the same main-menu PGC; that does not mean they
arrive with the same VM register values. Likewise, the French observations do
not establish that every submenu asset is French or that all audio and
subtitle tracks are set to French. Determining the precise localization rule
for each asset would require the disc's IFO commands and matching stream
metadata.

The UK trace also exposes the branch condition itself. Post-command 3,
`00a1000f000a0009`, means `if GPRM[15] == 10, goto line 9`. With the UK
button's value of `1`, that condition is false; execution falls through to
line 6 (clear `GPRM[15]`) and line 7 (jump to the menu path). The trace
establishes the branch mechanism, but not why the publisher chose this policy
or whether it was deliberate.

Do not add a special case to make the UK path play the French warnings or to
make France jump directly to the main menu. The player should present the
sectors and honor the waits and stills selected by the disc.
