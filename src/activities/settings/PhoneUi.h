#pragma once

#include <GfxRenderer.h>

#include <cstring>

// Draws `text` centered and word-wrapped to `maxWidth`, starting at `y`, for
// the iPhone screens' status and error details. Returns the y below the text.
inline int drawWrappedCentered(const GfxRenderer& renderer, const int fontId, int y, const char* text,
                               const int maxWidth, const int maxLines) {
  const int lineH = renderer.getLineHeight(fontId);
  char line[192];
  const char* rest = text;
  for (int row = 0; row < maxLines && *rest != '\0'; row++) {
    // Longest run of whole words that fits; a single overlong word is kept.
    size_t best = 0;
    size_t from = 0;
    while (true) {
      const char* space = strchr(rest + from, ' ');
      size_t candidate = space ? static_cast<size_t>(space - rest) : strlen(rest);
      if (candidate >= sizeof(line)) candidate = sizeof(line) - 1;
      memcpy(line, rest, candidate);
      line[candidate] = '\0';
      if (best > 0 && renderer.getTextWidth(fontId, line) > maxWidth) break;
      best = candidate;
      if (!space || candidate == sizeof(line) - 1) break;
      from = candidate + 1;
    }
    line[best] = '\0';
    renderer.drawCenteredText(fontId, y, line);
    y += lineH;
    rest += best;
    while (*rest == ' ') rest++;
  }
  return y;
}
