#pragma pass outline_flood vertex fragment
#pragma pass outline_resolve vertex fragment blend depth_lequal no_depth_write
#pragma pass outline_resolve_behind vertex fragment blend depth_greater no_depth_write

// The engine's outline shader: the screen-space half of the jump-flood
// outline, whose every line lives in the include (engine/outline.slang).
// The editor and a game load this same shader, and the graph they build
// around it decides what it draws: the resolve pass is the outline where
// the object is visible, the behind pass the fainter one where something
// occludes it, and a game that does not want outlines through walls just
// never adds that pass. The mask that feeds them comes from elsewhere,
// since what is outlined is not the shader's business: a game marks the
// objects by giving their own shader an `outline_mask` pass and masks with
// a DRAW_SCENE pass over it, the editor draws its selection through
// render/editor/selection.slang in a pass of its own.
//
// render/outline.hpp builds and drives the whole chain.

#include "engine/outline.slang"
