ENHANCED EDITION title words - layered PSDs (v0.8.7)

title_art_us.psd   under the US RAW DANGER logo (yellow, red rim, dark inner edge, leaning like the logo)
title_art_jp.psd   over the Japanese logo (white, soft dark edge)

Canvas: 2720 x 944 = the title picture's corner from x 300, y 330 (340 x 118 game pixels) at 8 times the size.
Layers: "Guide" (the title picture, for placement), "In the game" (hidden: what the build makes of it at the game's
640 x 448, enlarged), "Words" (rim, fill, the dark edge on the right and bottom), "Build number" (placement only: the build
draws the number itself, after the words). The words are the same size and place as in the game; each 8 x 8 block here is one
pixel there, so fine detail smaller than 8 pixels will not show in the game.

To use your version: hide "Guide" and "Build number", export as PNG (RGBA, 8 bits, same size) to
  tools\title_art_us.png   or   tools\title_art_jp.png
in the project, then build the Enhanced Edition again. The build makes it 8 times smaller and uses the nearest colours of the
title picture's 256-colour palette. Leave room at the right for the build number.
